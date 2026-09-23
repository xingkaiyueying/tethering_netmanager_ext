/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "nearlink_ipshare_client.h"

#include "nearlink_ipshare_controller.h"
#include "nearlink_family_validation.h"
#include <thread>

#include <cerrno>
#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cstring>
#include <future>
#include <set>
#include <net/if.h>

#include "dhcp_c_api.h"
#include "dhcp_result_event.h"
#include "net_conn_client.h"
#include "net_conn_callback_stub.h"
#include "nearlink_host.h"
#include "net_manager_constants.h"
#include "netmgr_ext_log_wrapper.h"
#include "netsys_controller.h"
#include "networkshare_tracker.h"
#include "networkshare_admission.h"
#include "securec.h"

namespace OHOS::NetManagerStandard {
namespace {
constexpr const char *IFACE_NAME = "sleip0";
constexpr const char *SUBNET_MASK = "255.255.255.0";
constexpr const char *FORWARDING_REQUESTER = "NearlinkIpShare";
constexpr int32_t PREFIX_LENGTH = 24;
constexpr int32_t DHCP_IPV4 = 0;
// Existing DHCP callback status ABI (dhcp_define.h EnumErrCode); no new C ABI.
constexpr int32_t DHCP_RENEW_FAILED = 4;
constexpr int32_t DHCP_RENEW_TIMEOUT = 5;
constexpr int32_t NETWORK_SCORE = 60;
constexpr int32_t IP_SHARE_LOCAL_NET_ID = 99;
constexpr const char *LOCAL_SUBNET = "192.168.77.0/24";
constexpr const char *DIRECT_NEXT_HOP = "0.0.0.0";

// The gateway uses the Demo subnet; terminals accept valid IPv4 DHCP subnets.
// DHCP success alone does not validate addresses, route or DNS readiness.
// Derive the parcel type from Netsys: internal and open-source trees use
// different namespaces for InterfaceConfigurationParcel.
template <typename Config> bool IsGatewayAddressAbsent(int32_t (NetsysController::*query)(Config &))
{
    Config config{};
    config.ifName = IFACE_NAME;
    return (NetsysController::GetInstance().*query)(config) == 0 &&
           (config.ipv4Addr.empty() || config.ipv4Addr == "0.0.0.0");
}

bool IsMissingDns(const char *value)
{
    // DHCP FormatString uses "*" for an absent optional DNS address.
    return value[0] == '\0' || strcmp(value, "*") == 0;
}

bool ReadIpv4(const char *text, uint32_t &host)
{
    in_addr address{};
    if (inet_pton(AF_INET, text, &address) != 1) {
        return false;
    }
    host = ntohl(address.s_addr);
    return true;
}

bool IsUnicastIpv4(uint32_t address)
{
    return (address >> 24) != 0 && (address >> 24) != 127 && (address >> 24) < 224;
}

bool HasValidLeaseDns(const DhcpResult &result)
{
    bool haveDns = false;
    for (const char *dns : {result.strOptDns1, result.strOptDns2}) {
        if (IsMissingDns(dns)) {
            continue;
        }
        uint32_t address = 0;
        if (!ReadIpv4(dns, address) || !IsUnicastIpv4(address)) {
            return false;
        }
        haveDns = true;
    }
    return haveDns;
}

bool ValidLease(const DhcpResult &result)
{
    for (const char *value :
         {result.strOptClientId, result.strOptSubnet, result.strOptRouter1, result.strOptDns1, result.strOptDns2}) {
        if (memchr(value, '\0', DHCP_MAX_FILE_BYTES) == nullptr) {
            NETMGR_EXT_LOG_E("[NearlinkIpShare][Lease] unterminated field");
            return false;
        }
    }
    NETMGR_EXT_LOG_I("[NearlinkIpShare][Lease] success=%{public}d type=%{public}d seconds=%{public}u "
                     "address=%{public}s mask=%{public}s router=%{public}s dns1=%{public}s dns2=%{public}s",
                     result.isOptSuc, result.iptype, result.uOptLeasetime, result.strOptClientId, result.strOptSubnet,
                     result.strOptRouter1, result.strOptDns1, result.strOptDns2);
    uint32_t ip, mask, router;
    if (!result.isOptSuc || result.iptype != DHCP_IPV4 || result.uOptLeasetime == 0 ||
        !ReadIpv4(result.strOptClientId, ip) || !ReadIpv4(result.strOptSubnet, mask) ||
        !ReadIpv4(result.strOptRouter1, router) || !IsUnicastIpv4(ip) || !IsUnicastIpv4(router)) {
        return false;
    }
    uint32_t hosts = ~mask;
    if (mask == 0 || hosts < 3 || (hosts & (hosts + 1)) != 0 || (ip & mask) != (router & mask) || ip == router ||
        (ip & hosts) == 0 || (ip & hosts) == hosts || (router & hosts) == 0 || (router & hosts) == hosts) {
        return false;
    }
    return HasValidLeaseDns(result);
}

int32_t LeasePrefix(const DhcpResult &result)
{
    in_addr mask{};
    inet_pton(AF_INET, result.strOptSubnet, &mask);
    uint32_t bits = ntohl(mask.s_addr);
    int32_t prefix = 0;
    while (bits & 0x80000000u) {
        ++prefix;
        bits <<= 1;
    }
    return prefix;
}

std::string LeaseSubnet(const DhcpResult &result)
{
    in_addr address{}, mask{};
    inet_pton(AF_INET, result.strOptClientId, &address);
    inet_pton(AF_INET, result.strOptSubnet, &mask);
    address.s_addr &= mask.s_addr;
    char text[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &address, text, sizeof(text));
    return text;
}

void PopulateTerminalLink(const DhcpResult &result, NetLinkInfo &linkInfo)
{
    linkInfo.ifaceName_ = IFACE_NAME;
    linkInfo.mtu_ = 1500;
    INetAddr address;
    address.type_ = INetAddr::IPV4;
    address.family_ = AF_INET;
    address.address_ = result.strOptClientId;
    address.netMask_ = result.strOptSubnet;
    address.prefixlen_ = LeasePrefix(result);
    linkInfo.netAddrList_.push_back(address);

    Route direct;
    direct.iface_ = IFACE_NAME;
    direct.destination_.type_ = INetAddr::IPV4;
    direct.destination_.family_ = AF_INET;
    direct.destination_.address_ = LeaseSubnet(result);
    direct.destination_.prefixlen_ = LeasePrefix(result);
    direct.gateway_.type_ = INetAddr::IPV4;
    direct.gateway_.family_ = AF_INET;
    direct.gateway_.address_ = DIRECT_NEXT_HOP;
    direct.hasGateway_ = false;
    linkInfo.routeList_.push_back(direct);

    Route route;
    route.iface_ = IFACE_NAME;
    route.isDefaultRoute_ = true;
    route.hasGateway_ = true;
    route.destination_.type_ = INetAddr::IPV4;
    route.destination_.family_ = AF_INET;
    route.destination_.address_ = "0.0.0.0";
    route.destination_.prefixlen_ = 0;
    route.gateway_.type_ = INetAddr::IPV4;
    route.gateway_.family_ = AF_INET;
    route.gateway_.address_ = result.strOptRouter1;
    linkInfo.routeList_.push_back(route);
    for (const char *dnsAddress : {result.strOptDns1, result.strOptDns2}) {
        if (!IsMissingDns(dnsAddress)) {
            INetAddr dns;
            dns.type_ = INetAddr::IPV4;
            dns.family_ = AF_INET;
            dns.address_ = dnsAddress;
            linkInfo.dnsList_.push_back(dns);
        }
    }
    linkInfo.isUserDefinedDnsServer_ = true;
}

class UpstreamCallback final : public NetConnCallbackStub {
public:
    int32_t NetAvailable(sptr<NetHandle> &) override
    {
        return Changed();
    }
    int32_t NetLost(sptr<NetHandle> &) override
    {
        return Changed();
    }
    int32_t NetUnavailable() override
    {
        return Changed();
    }
    int32_t NetConnectionPropertiesChange(sptr<NetHandle> &, const sptr<NetLinkInfo> &) override
    {
        return Changed();
    }

private:
    int32_t Changed()
    {
        NearlinkIpShareController::GetInstance()->OnUpstreamChanged();
        return NETMANAGER_SUCCESS;
    }
};

int32_t HexValue(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

class NearlinkObserver final : public OHOS::Nearlink::NearlinkIpShareObserver {
public:
    explicit NearlinkObserver(const std::weak_ptr<NearlinkIpShareController> &controller) : controller_(controller) {}

    void OnStatusChanged(const OHOS::Nearlink::NearlinkIpShareStatus &status) override
    {
        auto controller = controller_.lock();
        if (controller != nullptr) {
            controller->OnNearlinkStatus(status);
        }
    }

private:
    std::weak_ptr<NearlinkIpShareController> controller_;
};

void DhcpSessionSuccess(uint64_t session, int status, const char *iface, const DhcpResult *result,
                        const DhcpL3Ipv6Snapshot *snapshot)
{
    if (!iface || !result) {
        return;
    }
    auto controller = NearlinkIpShareController::GetInstance();
    if (snapshot) {
        controller->OnIpv6Addresses(iface, *snapshot, session);
    }
    controller->OnDhcpSuccess(status, iface, *result, session);
}
void DhcpSessionFailure(uint64_t session, int status, const char *iface, const char *reason)
{
    if (iface) {
        NearlinkIpShareController::GetInstance()->OnDhcpFailure(status, iface, reason ? reason : "", session);
    }
}
} // namespace

std::shared_ptr<NearlinkIpShareController> NearlinkIpShareController::GetInstance()
{
    static auto instance = std::shared_ptr<NearlinkIpShareController>(new NearlinkIpShareController());
    return instance;
}

bool NearlinkIpShareController::Init()
{
    std::lock_guard initLock(initMutex_);
    std::shared_ptr<OHOS::Nearlink::NearlinkIpShareObserver> observer;
    {
        std::lock_guard lock(mutex_);
        if (shuttingDown_) {
            return false;
        }
        if (nearlinkObserver_ == nullptr) {
            nearlinkObserver_ =
                std::make_shared<NearlinkObserver>(std::weak_ptr<NearlinkIpShareController>(shared_from_this()));
        }
        observer = nearlinkObserver_;
    }
    // The NearLink profile service is recreated when the adapter is toggled.
    // Its observer slot is process-local, while this controller remains alive,
    // so refresh the registration on every idempotent initialization.
    // RegisterObserver synchronously replays GetStatus through OnNearlinkStatus,
    // which also takes mutex_. Never hold the state lock across this call.
    int32_t ret = OHOS::Nearlink::NearlinkIpShareClient::GetInstance().RegisterObserver(observer);
    if (ret != 0) {
        NETMGR_EXT_LOG_E("[NearlinkIpShare][Init] observer registration failed code=%{public}d", ret);
        return false;
    }
    uint64_t generation;
    {
        std::lock_guard lock(mutex_);
        initialized_ = true;
        generation = generation_.load();
    }
    auto self = shared_from_this();
    NetworkShareTracker::GetInstance().SubmitNearlinkTask([self, generation]() {
        if (!self->IsCurrentSession(generation)) {
            return;
        }
        OHOS::Nearlink::NearlinkIpShareStatus snapshot;
        if (OHOS::Nearlink::NearlinkIpShareClient::GetInstance().GetStatus(snapshot) == 0) {
            self->HandleNearlinkStatus(snapshot);
        }
    });
    NETMGR_EXT_LOG_I("[NearlinkIpShare][Init] controller ready; observer registration refreshed");
    return true;
}

void NearlinkIpShareController::Uninit()
{
    std::lock_guard initLock(initMutex_);
    {
        std::lock_guard lock(mutex_);
        shuttingDown_ = true;
        stopRequested_ = true;
        ++generation_;
    }
    // Drain previously accepted work before touching worker-owned resources.
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    if (NetworkShareTracker::GetInstance().SubmitNearlinkTask([this, done]() {
            Cleanup();
            done->set_value();
        })) {
        future.get();
    }
    OHOS::Nearlink::NearlinkIpShareClient::GetInstance().UnregisterObserver();
    std::lock_guard lock(mutex_);
    initialized_ = false;
    nearlinkObserver_.reset();
    shuttingDown_ = false;
}

bool NearlinkIpShareController::ParsePeerAddress(const std::string &address, std::array<uint8_t, 6> &bytes)
{
    if (address.size() != 17) {
        return false;
    }
    for (size_t i = 0; i < bytes.size(); ++i) {
        size_t offset = i * 3;
        int32_t high = HexValue(address[offset]);
        int32_t low = HexValue(address[offset + 1]);
        if (high < 0 || low < 0 || (i + 1 < bytes.size() && address[offset + 2] != ':')) {
            return false;
        }
        bytes[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

std::string NearlinkIpShareController::MaskPeer(const std::string &address)
{
    return address.size() == 17 ? address.substr(0, 2) + ":**:**:**:**:" + address.substr(15, 2) : "invalid";
}

int32_t NearlinkIpShareController::IsSupported(const std::string &peerAddress, bool &supported)
{
    std::array<uint8_t, 6> bytes{};
    if (!ParsePeerAddress(peerAddress, bytes)) {
        NETMGR_EXT_LOG_E("[NearlinkIpShare][Support] invalid peer address");
        return NETMANAGER_EXT_ERR_PARAMETER_ERROR;
    }
    if (!Init()) {
        return NETMANAGER_EXT_ERR_OPERATION_FAILED;
    }
    auto result = std::make_shared<std::promise<std::pair<int32_t, bool>>>();
    auto future = result->get_future();
    if (!NetworkShareTracker::GetInstance().SubmitNearlinkTask([peerAddress, result]() {
            bool taskSupported = false;
            int32_t code =
                OHOS::Nearlink::NearlinkIpShareClient::GetInstance().IsPeerSupported(peerAddress, taskSupported);
            result->set_value({code, taskSupported});
        })) {
        return NETMANAGER_EXT_ERR_OPERATION_FAILED;
    }
    auto [ret, taskSupported] = future.get();
    supported = taskSupported;
    NETMGR_EXT_LOG_I("[NearlinkIpShare][Support] peer=%{public}s accepted=%{public}d code=%{public}d",
                     MaskPeer(peerAddress).c_str(), supported, ret);
    return ret;
}

int32_t NearlinkIpShareController::QueryCapabilities(const std::string &peer, NearlinkIpShareCapabilities &capabilities)
{
    std::array<uint8_t, 6> bytes{};
    if (!ParsePeerAddress(peer, bytes)) {
        return NETMANAGER_EXT_ERR_PARAMETER_ERROR;
    }
    if (!Init()) {
        return NETMANAGER_EXT_ERR_OPERATION_FAILED;
    }
    auto done = std::make_shared<std::promise<int32_t>>();
    auto future = done->get_future();
    auto value = std::make_shared<OHOS::Nearlink::NearlinkIpShareCapabilities>();
    if (!NetworkShareTracker::GetInstance().SubmitNearlinkTask([peer, done, value]() {
            done->set_value(
                OHOS::Nearlink::NearlinkIpShareClient::GetInstance().QueryNearlinkIpShareCapabilities(peer, *value));
        })) {
        return NETMANAGER_EXT_ERR_OPERATION_FAILED;
    }
    int32_t ret = future.get();
    if (ret == 0) {
        capabilities.identifierPresent = value->identifierPresent;
        capabilities.discoveryState = value->discoveryState;
        capabilities.localModes = value->localModes;
        capabilities.peerModes = value->peerModes;
        capabilities.peerCapabilityKnown = value->peerCapabilityKnown;
    }
    return ret;
}

int32_t NearlinkIpShareController::StartGateway(const std::string &peerAddress, int32_t mode)
{
    return Start(NearlinkIpShareRole::GATEWAY, peerAddress, mode);
}

int32_t NearlinkIpShareController::StartTerminal(const std::string &gatewayAddress, int32_t mode)
{
    return Start(NearlinkIpShareRole::TERMINAL, gatewayAddress, mode);
}

int32_t NearlinkIpShareController::Start(NearlinkIpShareRole role, const std::string &peerAddress, int32_t mode)
{
    std::array<uint8_t, 6> bytes{};
    if ((mode != 1 && mode != 3) || !ParsePeerAddress(peerAddress, bytes)) {
        NETMGR_EXT_LOG_E("[NearlinkIpShare][Start] role=%{public}d invalid peer address", static_cast<int32_t>(role));
        return NETMANAGER_EXT_ERR_PARAMETER_ERROR;
    }
    if (!Init()) {
        NETMGR_EXT_LOG_E("[NearlinkIpShare][Start] controller initialization failed");
        return NETMANAGER_EXT_ERR_OPERATION_FAILED;
    }
    std::lock_guard lock(mutex_);
    {
        if (!initialized_ || shuttingDown_) {
            NETMGR_EXT_LOG_E("[NearlinkIpShare][Start] role=%{public}d controller unavailable",
                             static_cast<int32_t>(role));
            return NETMANAGER_EXT_ERR_OPERATION_FAILED;
        }
        if (status_.role != NearlinkIpShareRole::NONE) {
            if (status_.role == role && status_.peerAddress == peerAddress && status_.requestedMode == mode &&
                !stopRequested_ && status_.state != NearlinkIpShareState::ERROR) {
                auto snapshot = status_;
                NetworkShareTracker::GetInstance().SubmitNearlinkTask(
                    [snapshot]() { NetworkShareTracker::GetInstance().SendNearlinkStateChange(snapshot); });
                return NETMANAGER_EXT_SUCCESS;
            }
            NETMGR_EXT_LOG_E("[NearlinkIpShare][Start] busy currentRole=%{public}d requestedRole=%{public}d",
                             static_cast<int32_t>(status_.role), static_cast<int32_t>(role));
            return NETMANAGER_EXT_ERR_OPERATION_FAILED;
        }
        if (role == NearlinkIpShareRole::GATEWAY) {
            if (!NetworkShareAdmission::GetInstance().AcquireNearlink()) {
                return NETMANAGER_EXT_ERR_OPERATION_FAILED;
            }
            gatewayReserved_ = true;
        }
        ++generation_;
        status_.requestedMode = mode;
        linkGeneration_ = linkSequence_ = evidenceSequence_ = 0;
        dualStack_ = false;
        channelReady_ = false;
        families_ = NearlinkFamilyNetwork{};
        pendingIpv4_ = NetLinkInfo{};
        ipv4PublishPending_ = false;
        networkDirty_ = false;
        ipv6Addresses_ = DhcpL3Ipv6Snapshot{};
        stopRequested_ = false;
        status_.state = NearlinkIpShareState::STARTING;
        status_.role = role;
        status_.peerAddress = peerAddress;
        status_.ifaceName.clear();
        status_.ipv4Address.clear();
        status_.hasUpstream = false;
        status_.errorStage.clear();
        status_.errorCode = 0;
    }
    auto self = shared_from_this();
    if (!NetworkShareTracker::GetInstance().SubmitNearlinkTask([self, role, peerAddress, mode]() {
            self->Publish(NearlinkIpShareState::STARTING);
            int32_t ret = role == NearlinkIpShareRole::GATEWAY
                              ? OHOS::Nearlink::NearlinkIpShareClient::GetInstance().StartNearlinkGatewayWithMode(
                                    peerAddress, mode)
                              : OHOS::Nearlink::NearlinkIpShareClient::GetInstance().StartNearlinkTerminalWithMode(
                                    peerAddress, mode);
            NETMGR_EXT_LOG_I("[NearlinkIpShare][Start] role=%{public}d peer=%{public}s accepted=%{public}d",
                             static_cast<int32_t>(role), MaskPeer(peerAddress).c_str(), ret);
            if (ret != 0) {
                self->Fail("LINK", ret);
            } else {
                self->nearlinkStarted_ = true;
            }
        })) {
        if (gatewayReserved_) {
            NetworkShareAdmission::GetInstance().ReleaseNearlink();
            gatewayReserved_ = false;
        }
        status_ = NearlinkIpShareStatus{};
        return NETMANAGER_EXT_ERR_OPERATION_FAILED;
    }
    return NETMANAGER_EXT_SUCCESS;
}

int32_t NearlinkIpShareController::StopGateway()
{
    return Stop(NearlinkIpShareRole::GATEWAY);
}

int32_t NearlinkIpShareController::StopTerminal()
{
    return Stop(NearlinkIpShareRole::TERMINAL);
}

int32_t NearlinkIpShareController::Stop(NearlinkIpShareRole expectedRole)
{
    std::lock_guard lock(mutex_);
    {
        if (status_.role == NearlinkIpShareRole::NONE) {
            return NETMANAGER_EXT_SUCCESS;
        }
        if (status_.role != expectedRole) {
            NETMGR_EXT_LOG_E("[NearlinkIpShare][Stop] role mismatch current=%{public}d requested=%{public}d",
                             static_cast<int32_t>(status_.role), static_cast<int32_t>(expectedRole));
            return NETMANAGER_EXT_ERR_PARAMETER_ERROR;
        }
        if (stopRequested_) {
            return NETMANAGER_EXT_SUCCESS;
        }
    }
    stopRequested_ = true;
    ++generation_;
    auto self = shared_from_this();
    if (!NetworkShareTracker::GetInstance().SubmitNearlinkTask([self]() {
            self->Publish(NearlinkIpShareState::STOPPING);
            self->Cleanup();
        })) {
        stopRequested_ = false;
        return NETMANAGER_EXT_ERR_OPERATION_FAILED;
    }
    return NETMANAGER_EXT_SUCCESS;
}

int32_t NearlinkIpShareController::GetStatus(NearlinkIpShareStatus &status) const
{
    std::lock_guard lock(mutex_);
    status = status_;
    return NETMANAGER_EXT_SUCCESS;
}

void NearlinkIpShareController::ReplayStatus(const sptr<INearlinkIpShareEventCallback> &callback) const
{
    if (callback == nullptr) {
        return;
    }
    NearlinkIpShareStatus status;
    GetStatus(status);
    callback->OnNearlinkIpShareStateChanged(status);
}

bool NearlinkIpShareController::IsCurrentSession(uint64_t generation) const
{
    std::lock_guard lock(mutex_);
    return generation == generation_ && !stopRequested_ && !shuttingDown_ &&
           status_.role != NearlinkIpShareRole::NONE && status_.state != NearlinkIpShareState::ERROR;
}

void NearlinkIpShareController::OnNearlinkStatus(const OHOS::Nearlink::NearlinkIpShareStatus &status)
{
    std::lock_guard lock(mutex_);
    auto self = shared_from_this();
    NetworkShareTracker::GetInstance().SubmitNearlinkTask([self, status, generation = generation_.load()]() {
        if (self->IsCurrentSession(generation)) {
            OHOS::Nearlink::NearlinkIpShareStatus current;
            if (OHOS::Nearlink::NearlinkIpShareClient::GetInstance().GetStatus(current) == 0 &&
                current.generation == status.generation) {
                self->HandleNearlinkStatus(status);
            }
        }
    });
}

void NearlinkIpShareController::HandleNearlinkStatus(const OHOS::Nearlink::NearlinkIpShareStatus &status)
{
    NearlinkIpShareRole role;
    {
        std::lock_guard lock(mutex_);
        role = status_.role;
        if (role == NearlinkIpShareRole::NONE || status_.state == NearlinkIpShareState::ERROR ||
            status_.state == NearlinkIpShareState::STOPPING) {
            return;
        }
        std::array<uint8_t, 6> expected{}, actual{};
        if (static_cast<int32_t>(status.role) != static_cast<int32_t>(role) ||
            !ParsePeerAddress(status.peerAddress, actual) || !ParsePeerAddress(status_.peerAddress, expected) ||
            actual != expected) {
            return;
        }
        if (!status.ifaceName.empty() && status.ifaceName != IFACE_NAME) {
            return;
        }
        if (status.generation < linkGeneration_ ||
            (status.generation == linkGeneration_ && status.sequence <= linkSequence_)) {
            return;
        }
        // A different live generation needs an explicit stop/restart; never graft old L3 resources onto it.
        if (linkGeneration_ != 0 && status.generation != linkGeneration_) {
            if (role != NearlinkIpShareRole::GATEWAY || interfaceIndex_ != if_nametoindex(IFACE_NAME)) {
                return;
            }
            // Peer release resets the gateway binding while keeping its TUN and local services alive.
            evidenceSequence_ = 0;
        }
        linkGeneration_ = status.generation;
        linkSequence_ = status.sequence;
        status_.contextId = status.contextId;
        status_.generation = status.generation;
        status_.selectedMode = static_cast<int32_t>(status.selectedMode);
        status_.serviceReady = status.serviceReady;
        dualStack_ = status_.selectedMode == 3;
        if (status.state == OHOS::Nearlink::NearlinkIpShareState::CHANNEL_READY) {
            channelReady_ = true;
        }
        if (status_.selectedMode == 0) {
            channelReady_ = false;
        }
        status_.ifaceName = status.ifaceName;
    }
    if (!dualStack_ && static_cast<int32_t>(status.selectedMode) == 1 && ipv6Prepared_) {
        if (ipv6Runtime_.Cleanup()) {
            ipv6Prepared_ = false;
            std::lock_guard lock(mutex_);
            status_.ipv6 = NearlinkIpShareFamilyStatus{};
        }
    }
    NETMGR_EXT_LOG_I("[NearlinkIpShare][NearLink] role=%{public}d state=%{public}d peer=%{public}s iface=%{public}s",
                     static_cast<int32_t>(role), static_cast<int32_t>(status.state),
                     MaskPeer(status.peerAddress).c_str(), status.ifaceName.c_str());
    if (status.state == OHOS::Nearlink::NearlinkIpShareState::ERROR) {
        Fail(status.errorStage.empty() ? "LINK" : status.errorStage, status.errorCode);
        return;
    }
    if (role == NearlinkIpShareRole::GATEWAY && (status.state == OHOS::Nearlink::NearlinkIpShareState::IFACE_READY ||
                                                 status.state == OHOS::Nearlink::NearlinkIpShareState::CHANNEL_READY)) {
        interfaceIndex_ = if_nametoindex(IFACE_NAME);
        ConfigureGateway();
        ConfigureUpstream();
        if (!maintenancePending_) {
            maintenancePending_ = true;
            ScheduleMaintenance(generation_);
        }
        return;
    }
    if (role == NearlinkIpShareRole::TERMINAL && status.state == OHOS::Nearlink::NearlinkIpShareState::CHANNEL_READY) {
        StartTerminalDhcp();
        return;
    }
    auto state = static_cast<NearlinkIpShareState>(static_cast<int32_t>(status.state));
    if (!addressConfigured_ && !dhcpClientStarted_ && state >= NearlinkIpShareState::STARTING &&
        state <= NearlinkIpShareState::CHANNEL_READY) {
        Publish(state);
    }
}

void NearlinkIpShareController::ConfigureGateway()
{
    const std::string &gateway = configuration_.GetNearlinkIpv4Addr();
    if (!localInterfaceAdded_) {
        int32_t ret = NetsysController::GetInstance().NetworkAddInterface(IP_SHARE_LOCAL_NET_ID, IFACE_NAME);
        if (ret != NETSYS_SUCCESS) {
            Fail("ROUTE", ret);
            return;
        }
        localInterfaceAdded_ = true;
    }
    // DNS proxy is shared by both families and must be attempted even if DHCPv4 fails.
    if (!dnsProxyStarted_) {
        dnsProxyStarted_ = NetsysController::GetInstance().StartDnsProxyListen() == NETSYS_SUCCESS;
    }
    int32_t ret = ConfigureGatewayIpv4();
    {
        std::lock_guard lock(mutex_);
        status_.ipv4.phase = ret == 0 ? 2 : 3;
        status_.ipv4.configurationAvailable = ret == 0;
        status_.ipv4.externalAvailable = false;
        status_.ipv4.hasError = ret != 0;
        status_.ipv4.error.plane = 4;
        status_.ipv4.error.family = 1;
        status_.ipv4.error.stage = "DHCP";
        status_.ipv4.error.code = ret;
        status_.ipv4.error.retryable = true;
        status_.ipv4Address = addressConfigured_ ? gateway : "";
        status_.serviceReady = dhcpServerStarted_;
        status_.ifaceName = IFACE_NAME;
    }
    if (!upstreamCallback_) {
        upstreamCallback_ = new (std::nothrow) UpstreamCallback();
        if (upstreamCallback_ && NetConnClient::GetInstance().RegisterNetConnCallback(upstreamCallback_) != 0) {
            upstreamCallback_ = nullptr;
        }
        // The maintenance task also rechecks the default upstream and retries registration.
    }
}

int32_t NearlinkIpShareController::ConfigureGatewayIpv4()
{
    const std::string &gateway = configuration_.GetNearlinkIpv4Addr();
    if (gateway != "192.168.77.1" || configuration_.GetNearlinkDhcpStart() != "192.168.77.2" ||
        configuration_.GetNearlinkDhcpEnd() != "192.168.77.20") {
        return NETMANAGER_EXT_ERR_PARAMETER_ERROR;
    }
    int32_t ret;
    if (!addressConfigured_) {
        ret = NetsysController::GetInstance().AddInterfaceAddress(IFACE_NAME, gateway, PREFIX_LENGTH);
        if (ret != NETSYS_SUCCESS) {
            return ret;
        }
        addressConfigured_ = true;
    }
    if (!localRouteAdded_) {
        ret = NetsysController::GetInstance().NetworkAddRoute(IP_SHARE_LOCAL_NET_ID, IFACE_NAME, LOCAL_SUBNET,
                                                              DIRECT_NEXT_HOP);
        if (ret != NETSYS_SUCCESS) {
            return ret;
        }
        localRouteAdded_ = true;
    }
    if (!dhcpServerStarted_) {
        DhcpRange range{};
        range.iptype = DHCP_IPV4;
        range.leaseHours = 6;
        if (strcpy_s(range.strTagName, sizeof(range.strTagName), IFACE_NAME) != EOK ||
            strcpy_s(range.strStartip, sizeof(range.strStartip), configuration_.GetNearlinkDhcpStart().c_str()) !=
                EOK ||
            strcpy_s(range.strEndip, sizeof(range.strEndip), configuration_.GetNearlinkDhcpEnd().c_str()) != EOK ||
            strcpy_s(range.strSubnet, sizeof(range.strSubnet), SUBNET_MASK) != EOK) {
            return NETMANAGER_EXT_ERR_PARAMETER_ERROR;
        }
        ret = SetDhcpRange(IFACE_NAME, &range);
        if (ret != DHCP_SUCCESS || (ret = StartDhcpServer(IFACE_NAME)) != DHCP_SUCCESS) {
            return ret;
        }
        dhcpServerStarted_ = true;
    }
    return NETSYS_SUCCESS;
}

void NearlinkIpShareController::OnUpstreamChanged()
{
    std::lock_guard lock(mutex_);
    auto self = shared_from_this();
    NetworkShareTracker::GetInstance().SubmitNearlinkTask([self, generation = generation_.load()]() {
        if (self->IsCurrentSession(generation) && self->localInterfaceAdded_) {
            self->ConfigureUpstream();
        }
    });
}

void NearlinkIpShareController::ConfigureUpstream()
{
    if (!localInterfaceAdded_) {
        return;
    }
    NetHandle upstream;
    auto link = sptr<NetLinkInfo>::MakeSptr();
    bool present = link && NetConnClient::GetInstance().GetDefaultNet(upstream) == NETMANAGER_SUCCESS &&
                   upstream.GetNetId() >= 0 &&
                   NetConnClient::GetInstance().GetConnectionProperties(upstream, *link) == 0 &&
                   !link->ifaceName_.empty() && link->ifaceName_ != IFACE_NAME;
    if (!present ||
        (upstreamNetId_ >= 0 && (upstreamNetId_ != upstream.GetNetId() || upstreamIface_ != link->ifaceName_))) {
        int32_t ret = CleanupUpstream();
        if (ret != 0) {
            ConfigureGatewayIpv6(nullptr);
            Publish(NearlinkIpShareState::SERVING_NO_UPSTREAM, "UPSTREAM", ret);
            return;
        }
    }
    if (!present) {
        {
            std::lock_guard lock(mutex_);
            status_.hasUpstream = false;
        }
        ConfigureGatewayIpv6(nullptr);
        Publish(NearlinkIpShareState::SERVING_NO_UPSTREAM);
        return;
    }
    upstreamNetId_ = upstream.GetNetId();
    upstreamIface_ = link->ifaceName_;
    int32_t dnsRet = dnsProxyStarted_ ? NetsysController::GetInstance().ShareDnsSet(upstreamNetId_) : -1;
    dnsUpstreamReady_ = dnsRet == NETSYS_SUCCESS;
    int32_t ret = 0;
    if (!forwardingEnabled_) {
        ret = NetsysController::GetInstance().IpEnableForwarding(FORWARDING_REQUESTER);
        forwardingEnabled_ = ret == 0;
    }
    if (forwardingEnabled_ && !interfaceForwarding_) {
        ret = NetsysController::GetInstance().IpfwdAddInterfaceForward(IFACE_NAME, upstreamIface_);
        interfaceForwarding_ = ret == 0;
    }
    bool hasV4Route = std::any_of(link->routeList_.begin(), link->routeList_.end(), [](const auto &route) {
        return route.destination_.family_ == AF_INET && route.destination_.prefixlen_ == 0;
    });
    if (interfaceForwarding_ && hasV4Route && !natEnabled_) {
        ret = NetsysController::GetInstance().EnableNat(IFACE_NAME, upstreamIface_);
        natEnabled_ = ret == 0;
    } else if (!hasV4Route && natEnabled_) {
        ret = NetsysController::GetInstance().DisableNat(IFACE_NAME, upstreamIface_);
        if (ret == 0) {
            natEnabled_ = false;
        }
    }
    ConfigureGatewayIpv6(&*link);
    {
        std::lock_guard lock(mutex_);
        status_.hasUpstream = interfaceForwarding_ && forwardingEnabled_;
        if (dnsRet != 0 || (hasV4Route && !natEnabled_)) {
            status_.ipv4.hasError = true;
            status_.ipv4.error.plane = 4;
            status_.ipv4.error.family = 1;
            status_.ipv4.error.stage = dnsRet != 0 ? "DNS" : "NAT";
            status_.ipv4.error.code = dnsRet != 0 ? dnsRet : ret;
            status_.ipv4.error.retryable = true;
        }
    }
    Publish(interfaceForwarding_ ? NearlinkIpShareState::SERVING : NearlinkIpShareState::SERVING_NO_UPSTREAM);
}

void NearlinkIpShareController::ConfigureGatewayIpv6(const NetLinkInfo *upstream)
{
    if (!dualStack_ || !channelReady_) {
        return;
    }
    if (!ipv6Prepared_) {
        std::string local;
        int32_t ret = OHOS::Nearlink::NearlinkHost::GetInstance().GetLocalAddress(local);
        ipv6Prepared_ = ret == 0 && ipv6Runtime_.Prepare(true, local);
    }
    bool ready = ipv6Prepared_ &&
                 ipv6Runtime_.Advertise(upstream, interfaceForwarding_ && forwardingEnabled_, dnsUpstreamReady_);
    std::lock_guard lock(mutex_);
    status_.serviceReady = dhcpServerStarted_ || ready;
    status_.ipv6.phase = ready && dnsUpstreamReady_ ? 2 : 3;
    status_.ipv6.configurationAvailable = ready && dnsUpstreamReady_;
    status_.ipv6.externalAvailable = false;
    status_.ipv6.validation = 0;
    status_.ipv6.hasError = !ready || !dnsUpstreamReady_;
    if (status_.ipv6.hasError) {
        status_.ipv6.error.plane = 4;
        status_.ipv6.error.family = 2;
        status_.ipv6.error.stage = ready ? "DNS" : "PREFIX";
        status_.ipv6.error.retryable = true;
        status_.ipv6.error.code = NETMANAGER_EXT_ERR_OPERATION_FAILED;
    }
}

void NearlinkIpShareController::StartTerminalDhcp()
{
    if (dhcpClientStarted_) {
        return;
    }
    std::array<uint8_t, 6> clientKey{};
    std::string localAddress;
    int32_t addressRet = OHOS::Nearlink::NearlinkHost::GetInstance().GetLocalAddress(localAddress);
    if (addressRet != 0) {
        Fail("IDENTITY", addressRet);
        return;
    }
    if (!ParsePeerAddress(localAddress, clientKey) ||
        std::all_of(clientKey.begin(), clientKey.end(), [](uint8_t value) { return value == 0; })) {
        Fail("IDENTITY", NETMANAGER_EXT_ERR_PARAMETER_ERROR);
        return;
    }
    Publish(NearlinkIpShareState::DHCP);
    clientKey_ = clientKey;
    interfaceIndex_ = if_nametoindex(IFACE_NAME);
    int32_t ret = RegisterDhcpClientL3Session(IFACE_NAME, generation_, DhcpSessionSuccess, DhcpSessionFailure);
    if (ret != DHCP_SUCCESS) {
        Fail("DHCP", ret);
        return;
    }
    if (dualStack_) {
        ipv6Prepared_ = ipv6Runtime_.Prepare(false, localAddress);
        if (!ipv6Prepared_) {
            FamilyFailure(true, "IPV6_CONTROL", NETMANAGER_EXT_ERR_OPERATION_FAILED);
        }
    }
    dhcpClientStarted_ = true;
    retryIpv4_ = true;
    ipv6ClientStarted_ = false;
    nextDhcpRetry_ = std::chrono::steady_clock::time_point{};
    RetryTerminalDhcp();
    if (!maintenancePending_) {
        maintenancePending_ = true;
        ScheduleMaintenance(generation_);
    }
    NETMGR_EXT_LOG_I("[NearlinkIpShare][Terminal] DHCP L3_TUN started interface=%{public}s", IFACE_NAME);
}

void NearlinkIpShareController::RetryTerminalDhcp()
{
    auto now = std::chrono::steady_clock::now();
    if (now < nextDhcpRetry_) {
        return;
    }
    nextDhcpRetry_ = now + std::chrono::seconds(30);
    RouterConfig config{};
    if (strcpy_s(config.ifname, sizeof(config.ifname), IFACE_NAME) != EOK) {
        return;
    }
    config.prohibitUseCacheIp = true;
    // Separate starts prevent an IPv6 startup error from skipping DHCPv4, and vice versa.
    if (dualStack_ && !ipv6ClientStarted_) {
        if (!ipv6Prepared_) {
            std::string local;
            if (OHOS::Nearlink::NearlinkHost::GetInstance().GetLocalAddress(local) == 0) {
                ipv6Prepared_ = ipv6Runtime_.Prepare(false, local);
            }
        }
        if (ipv6Prepared_) {
            config.bIpv6 = true;
            config.bIpv4 = false;
            int32_t ret = StartDhcpClientL3(&config, clientKey_.data(), clientKey_.size());
            ipv6ClientStarted_ = ret == DHCP_SUCCESS;
            if (ret != DHCP_SUCCESS) {
                FamilyFailure(true, "IPV6_CONTROL", ret);
            }
        }
    }
    if (retryIpv4_) {
        config.bIpv6 = false;
        config.bIpv4 = true;
        int32_t ret = StartDhcpClientL3(&config, clientKey_.data(), clientKey_.size());
        retryIpv4_ = ret != DHCP_SUCCESS;
        if (ret != DHCP_SUCCESS) {
            FamilyFailure(false, "DHCP", ret);
        }
    }
}

void NearlinkIpShareController::OnDhcpSuccess(int32_t status, const std::string &iface, const DhcpResult &result,
                                              uint64_t session)
{
    NETMGR_EXT_LOG_I("[NearlinkIpShare][DHCP] async success callback interface=%{public}s code=%{public}d",
                     iface.c_str(), status);
    std::lock_guard lock(mutex_);
    if (session != 0 && session != generation_) {
        return;
    }
    auto self = shared_from_this();
    NetworkShareTracker::GetInstance().SubmitNearlinkTask([self, status, iface, result,
                                                           generation = generation_.load()]() {
        if (!self->IsCurrentSession(generation) || !self->dhcpClientStarted_ || iface != IFACE_NAME) {
            return;
        }
        if (result.iptype == 1) {
            if (self->dualStack_) {
                self->ApplyIpv6Network(result);
            }
            return;
        }
        if (status != DHCP_SUCCESS || !ValidLease(result)) {
            self->FamilyFailure(false, "DHCP", status == DHCP_SUCCESS ? NETMANAGER_EXT_ERR_PARAMETER_ERROR : status);
            return;
        }
        self->ApplyTerminalNetwork(result);
    });
}

void NearlinkIpShareController::OnDhcpFailure(int32_t status, const std::string &iface, const std::string &reason,
                                              uint64_t session)
{
    (void)reason;
    NETMGR_EXT_LOG_E("[NearlinkIpShare][DHCP] async failure callback interface=%{public}s code=%{public}d",
                     iface.c_str(), status);
    std::lock_guard lock(mutex_);
    if (session != 0 && session != generation_) {
        return;
    }
    auto self = shared_from_this();
    NetworkShareTracker::GetInstance().SubmitNearlinkTask([self, status, iface, generation = generation_.load()]() {
        if (self->IsCurrentSession(generation) && self->dhcpClientStarted_ && iface == IFACE_NAME) {
            if ((status == DHCP_RENEW_FAILED || status == DHCP_RENEW_TIMEOUT) &&
                ((self->netSupplierId_ != 0 && std::chrono::steady_clock::now() < self->leaseExpiry_) ||
                 (self->ipv4PublishPending_ && std::chrono::steady_clock::now() < self->pendingLeaseExpiry_))) {
                NETMGR_EXT_LOG_I("[NearlinkIpShare][DHCP] temporary renewal failure; valid lease retained");
                return;
            }
            if (status & 0x10000) {
                if (!NearlinkFamilyNetwork::Available(self->families_.ipv6)) {
                    self->FamilyFailure(true, "IPV6_CONTROL", status & 0xffff);
                }
            } else {
                self->ipv4PublishPending_ = false;
                self->retryIpv4_ = true;
                self->nextDhcpRetry_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                self->FamilyFailure(false, "DHCP", status);
            }
        }
    });
}

void NearlinkIpShareController::ApplyTerminalNetwork(const DhcpResult &result)
{
    retryIpv4_ = false;
    pendingIpv4_ = NetLinkInfo{};
    PopulateTerminalLink(result, pendingIpv4_);
    pendingLeaseExpiry_ = std::chrono::steady_clock::now() + std::chrono::seconds(result.uOptLeasetime);
    ipv4PublishPending_ = true;
    RetryTerminalNetwork();
}

void NearlinkIpShareController::RetryTerminalNetwork()
{
    if (!ipv4PublishPending_) {
        if (networkDirty_) {
            PublishTerminalNetwork();
        }
        return;
    }
    if (std::chrono::steady_clock::now() >= pendingLeaseExpiry_) {
        ipv4PublishPending_ = false;
        pendingIpv4_ = NetLinkInfo{};
        retryIpv4_ = true;
        return;
    }
    auto previous = families_.ipv4;
    families_.ipv4 = pendingIpv4_;
    if (!PublishTerminalNetwork()) {
        families_.ipv4 = previous;
        FamilyFailure(false, "ROUTE", NETMANAGER_EXT_ERR_OPERATION_FAILED, false);
        return;
    }
    ipv4PublishPending_ = false;
    pendingIpv4_ = NetLinkInfo{};
    {
        std::lock_guard lock(mutex_);
        status_.ipv4.hasError = false;
    }
    leaseExpiry_ = families_.leaseExpiry = pendingLeaseExpiry_;
    RefreshFamilyStatus();
}

bool NearlinkIpShareController::PublishTerminalNetwork()
{
    bool dirty = networkDirty_;
    networkDirty_ = true;
    bool created = netSupplierId_ == 0;
    if (created && NetConnClient::GetInstance().RegisterNetSupplier(BEARER_BLUETOOTH, IFACE_NAME,
                                                                    {NET_CAPABILITY_INTERNET, NET_CAPABILITY_NOT_VPN},
                                                                    netSupplierId_) != NETMANAGER_SUCCESS) {
        return false;
    }
    auto link = sptr<NetLinkInfo>::MakeSptr(families_.Aggregate());
    auto supplier = sptr<NetSupplierInfo>::MakeSptr();
    if (!link || !supplier) {
        return false;
    }
    // Supplier availability tracks the live shared interface, not an individual DHCP/DNS result.
    // Dropping it for a DNS timeout would destroy the network and its other kernel-managed addresses.
    supplier->isAvailable_ = true;
    supplier->score_ = NETWORK_SCORE;
    if (!dirty && !created && supplierAvailable_ == supplier->isAvailable_ && appliedLink_ == *link) {
        networkDirty_ = false;
        return true;
    }
    // NetConn creates the physical network when supplier availability becomes true.
    // Install link resources only after that step; on failure restore the previous aggregate.
    int32_t ret = NETMANAGER_SUCCESS;
    if (supplier->isAvailable_ && !supplierAvailable_) {
        ret = NetConnClient::GetInstance().UpdateNetSupplierInfo(netSupplierId_, supplier);
    }
    if (ret == NETMANAGER_SUCCESS) {
        ret = NetConnClient::GetInstance().UpdateNetLinkInfo(netSupplierId_, link);
    }
    if (ret == NETMANAGER_SUCCESS) {
        ret = NetConnClient::GetInstance().UpdateNetSupplierInfo(netSupplierId_, supplier);
    }
    if (ret != NETMANAGER_SUCCESS) {
        if (created) {
            if (NetConnClient::GetInstance().UnregisterNetSupplier(netSupplierId_) == NETMANAGER_SUCCESS) {
                netSupplierId_ = 0;
            }
        } else {
            auto restore = sptr<NetLinkInfo>::MakeSptr(appliedLink_);
            auto previous = sptr<NetSupplierInfo>::MakeSptr();
            if (restore && previous) {
                previous->isAvailable_ = supplierAvailable_;
                previous->score_ = NETWORK_SCORE;
                (void)NetConnClient::GetInstance().UpdateNetLinkInfo(netSupplierId_, restore);
                (void)NetConnClient::GetInstance().UpdateNetSupplierInfo(netSupplierId_, previous);
            }
        }
        return false;
    }
    networkDirty_ = false;
    appliedLink_ = *link;
    ++networkRevision_;
    nextValidation_ = std::chrono::steady_clock::time_point{};
    {
        std::lock_guard lock(mutex_);
        status_.ipv4.externalAvailable = status_.ipv6.externalAvailable = false;
        status_.ipv4.validation = status_.ipv6.validation = 0;
    }
    supplierAvailable_ = supplier->isAvailable_;
    std::list<int32_t> ids;
    if (NetConnClient::GetInstance().GetNetIdByIdentifier(IFACE_NAME, ids) == NETMANAGER_SUCCESS && ids.size() == 1) {
        std::lock_guard lock(mutex_);
        status_.netId = ids.front();
    }
    return true;
}

void NearlinkIpShareController::OnIpv6Addresses(const std::string &iface, const DhcpL3Ipv6Snapshot &snapshot,
                                                uint64_t session)
{
    if (iface != IFACE_NAME || snapshot.addressCount > DHCP_L3_IPV6_MAX_ADDRESSES) {
        return;
    }
    std::lock_guard lock(mutex_);
    if (session != 0 && session != generation_) {
        return;
    }
    auto self = shared_from_this();
    NetworkShareTracker::GetInstance().SubmitNearlinkTask([self, snapshot, generation = generation_.load()]() {
        if (!self->IsCurrentSession(generation) || !self->dhcpClientStarted_ || !self->dualStack_) {
            return;
        }
        for (uint32_t i = 0; i < snapshot.addressCount; ++i) {
            const auto &a = snapshot.addresses[i];
            in6_addr parsed{};
            if (!memchr(a.address, 0, sizeof(a.address)) || inet_pton(AF_INET6, a.address, &parsed) != 1 ||
                a.ifindex != self->interfaceIndex_ || a.prefixLength > 128 || a.preferredLifetime > a.validLifetime) {
                return;
            }
        }
        // Withdraw records missing from this complete snapshot before submitting new evidence.
        auto submit = [self](const DhcpL3Ipv6Address &a, bool removed) {
            OHOS::Nearlink::NearlinkIpShareAddressEvidence evidence;
            evidence.generation = self->linkGeneration_;
            evidence.sequence = ++self->evidenceSequence_;
            evidence.address = a.address;
            evidence.ifindex = a.ifindex;
            evidence.prefixLength = a.prefixLength;
            evidence.flags = a.flags;
            evidence.preferredLifetime = removed ? 0 : a.preferredLifetime;
            evidence.validLifetime = removed ? 0 : a.validLifetime;
            return OHOS::Nearlink::NearlinkIpShareClient::GetInstance().UpdateValidatedAddress(evidence);
        };
        for (uint32_t i = 0; i < self->ipv6Addresses_.addressCount; ++i) {
            const auto &old = self->ipv6Addresses_.addresses[i];
            bool found = false;
            for (uint32_t j = 0; j < snapshot.addressCount; ++j) {
                found = found || strcmp(old.address, snapshot.addresses[j].address) == 0;
            }
            if (!found && submit(old, true) != 0) {
                self->FamilyFailure(true, "IPV6_CONTROL", NETMANAGER_EXT_ERR_OPERATION_FAILED);
                return;
            }
        }
        auto accepted = snapshot;
        for (uint32_t i = 0; i < snapshot.addressCount; ++i) {
            // IPC runs on our worker, never nested under the DHCP Binder caller identity.
            if (submit(snapshot.addresses[i], false) != 0) {
                accepted.addresses[i].validLifetime = 0;
            }
        }
        self->ipv6Addresses_ = accepted;
        self->ipv6Observed_ = std::chrono::steady_clock::now();
    });
}

void NearlinkIpShareController::ApplyIpv6Network(const DhcpResult &result)
{
    if (!memchr(result.strOptRouter1, 0, sizeof(result.strOptRouter1)) ||
        result.dnsList.dnsNumber > DHCP_DNS_MAX_NUMBER) {
        FamilyFailure(true, "IPV6_CONTROL", NETMANAGER_EXT_ERR_PARAMETER_ERROR);
        return;
    }
    NetLinkInfo next;
    auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - ipv6Observed_).count();
    for (uint32_t i = 0; i < ipv6Addresses_.addressCount; ++i) {
        const auto &a = ipv6Addresses_.addresses[i];
        in6_addr binary{};
        if (a.validLifetime == 0 || (a.validLifetime != UINT32_MAX && elapsed >= a.validLifetime) ||
            (a.flags & (0x08 | 0x40 | 0x04)) || inet_pton(AF_INET6, a.address, &binary) != 1 ||
            ((binary.s6_addr[0] & 0xfe) != 0xfc && (binary.s6_addr[0] & 0xe0) != 0x20)) {
            continue;
        }
        INetAddr address;
        address.type_ = INetAddr::IPV6;
        address.family_ = AF_INET6;
        address.address_ = a.address;
        address.prefixlen_ = a.prefixLength;
        next.netAddrList_.push_back(address);
        for (uint32_t bit = a.prefixLength; bit < 128; ++bit) {
            binary.s6_addr[bit / 8] &= ~(0x80 >> (bit % 8));
        }
        char subnet[INET6_ADDRSTRLEN]{};
        inet_ntop(AF_INET6, &binary, subnet, sizeof(subnet));
        Route direct;
        direct.iface_ = IFACE_NAME;
        direct.destination_ = address;
        direct.destination_.address_ = subnet;
        direct.gateway_.type_ = INetAddr::IPV6;
        direct.gateway_.family_ = AF_INET6;
        direct.gateway_.address_ = "::";
        direct.hasGateway_ = false;
        if (std::none_of(next.routeList_.begin(), next.routeList_.end(), [&direct](const auto &route) {
                return route.destination_.address_ == direct.destination_.address_ &&
                       route.destination_.prefixlen_ == direct.destination_.prefixlen_;
            })) {
            next.routeList_.push_back(direct);
        }
    }
    in6_addr router{};
    if (result.ipv6LifeTime.routerLifeTime != 0 && inet_pton(AF_INET6, result.strOptRouter1, &router) == 1 &&
        IN6_IS_ADDR_LINKLOCAL(&router) && !next.netAddrList_.empty()) {
        Route route;
        route.iface_ = IFACE_NAME; // Netsys resolves the LLA next hop in this interface's scope.
        route.destination_.type_ = route.gateway_.type_ = INetAddr::IPV6;
        route.destination_.family_ = route.gateway_.family_ = AF_INET6;
        route.destination_.address_ = "::";
        route.destination_.prefixlen_ = 0;
        route.gateway_.address_ = result.strOptRouter1;
        route.isDefaultRoute_ = route.hasGateway_ = true;
        next.routeList_.push_back(route);
    }
    for (uint32_t i = 0; i < result.dnsList.dnsNumber && next.dnsList_.size() < 8; ++i) {
        auto text = result.dnsList.dnsAddr[i];
        in6_addr dns{};
        if (!memchr(text, 0, DHCP_DNS_DATA_MAX_LEN) || inet_pton(AF_INET6, text, &dns) != 1 ||
            IN6_IS_ADDR_UNSPECIFIED(&dns) || IN6_IS_ADDR_MULTICAST(&dns) || IN6_IS_ADDR_LINKLOCAL(&dns)) {
            continue;
        }
        INetAddr entry;
        entry.type_ = INetAddr::IPV6;
        entry.family_ = AF_INET6;
        entry.address_ = text;
        next.dnsList_.push_back(entry);
    }
    if (families_.ipv6 == next) {
        ipv6Result_ = result;
        return;
    }
    auto previous = families_.ipv6;
    families_.ipv6 = next;
    ipv6Result_ = result;
    if (!PublishTerminalNetwork()) {
        families_.ipv6 = previous;
        FamilyFailure(true, "ROUTE_V6", NETMANAGER_EXT_ERR_OPERATION_FAILED, false);
        return;
    }
    {
        std::lock_guard lock(mutex_);
        status_.ipv6.hasError = false;
    }
    RefreshFamilyStatus();
}

void NearlinkIpShareController::FamilyFailure(bool ipv6, const std::string &stage, int32_t code, bool withdraw)
{
    // Family failure never stops DHCP/SLAAC, the supplier or the shared NearLink channel.
    // A later callback retries the family on the same supplier.
    auto &link = ipv6 ? families_.ipv6 : families_.ipv4;
    if (withdraw) {
        link = NetLinkInfo{};
    }
    bool terminal;
    {
        std::lock_guard lock(mutex_);
        terminal = status_.role == NearlinkIpShareRole::TERMINAL;
        auto &family = ipv6 ? status_.ipv6 : status_.ipv4;
        family = NearlinkIpShareFamilyStatus{};
        family.phase = 3;
        family.hasError = true;
        family.error.plane = 4;
        family.error.family = ipv6 ? 2 : 1;
        family.error.stage = stage;
        family.error.code = code;
        family.error.retryable = true;
    }
    if (terminal) {
        if (netSupplierId_ != 0) {
            (void)PublishTerminalNetwork();
        }
        RefreshFamilyStatus();
    } else {
        Publish(NearlinkIpShareState::SERVING_NO_UPSTREAM, stage, code);
    }
}

void NearlinkIpShareController::RefreshFamilyStatus()
{
    bool available;
    {
        std::lock_guard lock(mutex_);
        auto fill = [this](NearlinkIpShareFamilyStatus &family, const NetLinkInfo &link, bool v6) {
            family.configurationAvailable = NearlinkFamilyNetwork::Available(link);
            if (!family.configurationAvailable) {
                family.externalAvailable = false;
                family.validation = 0;
            }
            if (family.configurationAvailable) {
                family.phase = 2;
            } else if (!family.hasError) {
                family.phase = v6 && !dualStack_ ? 0 : 1;
            }
            family.addresses.clear();
            family.routes.clear();
            family.dns.clear();
            for (const auto &a : link.netAddrList_) {
                NearlinkIpShareAddress value;
                value.address = a.address_;
                value.prefixLength = a.prefixlen_;
                value.scopeId = v6 ? interfaceIndex_ : 0;
                value.origin = v6 ? 2 : 1;
                value.dadState = v6 ? 2 : 0;
                auto now = std::chrono::steady_clock::now();
                auto left = [](uint32_t life, int64_t elapsed) -> uint32_t {
                    return life == UINT32_MAX ? life : (elapsed >= life ? 0 : life - elapsed);
                };
                if (v6) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - ipv6Observed_).count();
                    for (uint32_t i = 0; i < ipv6Addresses_.addressCount; ++i) {
                        const auto &entry = ipv6Addresses_.addresses[i];
                        if (a.address_ != entry.address) {
                            continue;
                        }
                        value.preferredLifetime = left(entry.preferredLifetime, elapsed);
                        value.validLifetime = left(entry.validLifetime, elapsed);
                        value.dadState = value.preferredLifetime ? 2 : 3;
                    }
                } else {
                    auto remaining = std::chrono::duration_cast<std::chrono::seconds>(leaseExpiry_ - now).count();
                    value.preferredLifetime = value.validLifetime = remaining > 0 ? remaining : 0;
                }
                family.addresses.push_back(value);
            }
            for (const auto &r : link.routeList_) {
                NearlinkIpShareRoute value;
                value.destination = r.destination_.address_;
                value.gateway = r.gateway_.address_;
                value.prefixLength = r.destination_.prefixlen_;
                value.scopeId = v6 ? interfaceIndex_ : 0;
                if (!v6 && !family.addresses.empty()) {
                    value.lifetimeKnown = true;
                    value.lifetime = family.addresses.front().validLifetime;
                }
                family.routes.push_back(value);
            }
            for (const auto &d : link.dnsList_) {
                NearlinkIpShareDns value;
                value.address = d.address_;
                value.transportFamily = v6 ? 2 : 1;
                value.source = v6 ? 2 : 1;
                // RDNSS expiry remains owned by DHCP's DNS ledger; its C snapshot has no per-record TTL.
                if (!v6 && !family.addresses.empty()) {
                    value.lifetimeKnown = true;
                    value.lifetime = family.addresses.front().validLifetime;
                }
                family.dns.push_back(value);
            }
        };
        fill(status_.ipv4, families_.ipv4, false);
        fill(status_.ipv6, families_.ipv6, true);
        status_.ipv4Address = families_.ipv4.netAddrList_.empty() ? "" : families_.ipv4.netAddrList_.front().address_;
        available = status_.ipv4.configurationAvailable || status_.ipv6.configurationAvailable;
        status_.hasUpstream = status_.ipv4.externalAvailable || status_.ipv6.externalAvailable;
    }
    Publish(available ? NearlinkIpShareState::ACTIVE : NearlinkIpShareState::CONFIGURING);
}

void NearlinkIpShareController::ValidateFamilies()
{
    auto now = std::chrono::steady_clock::now();
    if (validationInFlight_ || now < nextValidation_) {
        return;
    }
    int32_t netId;
    bool ipv4, ipv6;
    {
        std::lock_guard lock(mutex_);
        netId = status_.netId;
        ipv4 = status_.ipv4.configurationAvailable;
        ipv6 = status_.ipv6.configurationAvailable;
        if (netId < 0 || (!ipv4 && !ipv6)) {
            return;
        }
        if (ipv4) {
            status_.ipv4.validation = 1;
        }
        if (ipv6) {
            status_.ipv6.validation = 1;
        }
    }
    validationInFlight_ = true;
    nextValidation_ = now + std::chrono::seconds(30);
    auto self = shared_from_this();
    // DNS/HTTP must never block the shared lifecycle worker or delay stop.
    std::thread([self, netId, ipv4, ipv6, generation = generation_.load(), revision = networkRevision_]() {
        auto result = ValidateNearlinkFamilies(netId, ipv4, ipv6);
        NetworkShareTracker::GetInstance().SubmitNearlinkTask([self, result, generation, revision]() {
            if (!self->IsCurrentSession(generation)) {
                return;
            }
            self->validationInFlight_ = false;
            if (revision != self->networkRevision_) {
                return;
            }
            {
                std::lock_guard lock(self->mutex_);
                self->status_.ipv4.validation = result.ipv4;
                self->status_.ipv6.validation = result.ipv6;
                self->status_.ipv4.externalAvailable = result.ipv4 == 2 && self->status_.ipv4.configurationAvailable;
                self->status_.ipv6.externalAvailable = result.ipv6 == 2 && self->status_.ipv6.configurationAvailable;
            }
            self->RefreshFamilyStatus();
        });
    }).detach();
}

void NearlinkIpShareController::ScheduleMaintenance(uint64_t generation)
{
    auto self = shared_from_this();
    NetworkShareTracker::GetInstance().SubmitNearlinkTask(
        [self, generation]() {
            if (!self->IsCurrentSession(generation)) {
                return;
            }
            if (if_nametoindex(IFACE_NAME) != self->interfaceIndex_) {
                self->Fail("LINK", NETMANAGER_EXT_ERR_OPERATION_FAILED);
                return;
            }
            OHOS::Nearlink::NearlinkIpShareStatus current;
            if (OHOS::Nearlink::NearlinkIpShareClient::GetInstance().GetStatus(current) != 0 ||
                (current.generation != self->linkGeneration_ &&
                 current.role != OHOS::Nearlink::NearlinkIpShareRole::GATEWAY) ||
                current.state == OHOS::Nearlink::NearlinkIpShareState::IDLE ||
                current.state == OHOS::Nearlink::NearlinkIpShareState::ERROR) {
                self->Fail("LINK", NETMANAGER_EXT_ERR_OPERATION_FAILED);
                return;
            }
            if (current.role == OHOS::Nearlink::NearlinkIpShareRole::GATEWAY) {
                self->HandleNearlinkStatus(current);
                self->ConfigureGateway();
                self->ConfigureUpstream();
                self->ScheduleMaintenance(generation);
                return;
            }
            if (self->families_.ExpireLease(std::chrono::steady_clock::now())) {
                self->retryIpv4_ = true;
                self->FamilyFailure(false, "DHCP", DHCP_RENEW_TIMEOUT);
            }
            self->RetryTerminalNetwork();
            self->RetryTerminalDhcp();
            if (self->dualStack_) {
                self->ApplyIpv6Network(self->ipv6Result_);
            }
            self->ValidateFamilies();
            self->ScheduleMaintenance(generation);
        },
        1000000);
}

int32_t NearlinkIpShareController::CleanupUpstream()
{
    dnsUpstreamReady_ = false;
    int32_t error = NETSYS_SUCCESS;
    auto release = [&error](bool &owned, int32_t ret) {
        if (ret == NETSYS_SUCCESS) {
            owned = false;
        } else if (error == NETSYS_SUCCESS) {
            error = ret;
        }
        NETMGR_EXT_LOG_I("[NearlinkIpShare][Cleanup] upstream release code=%{public}d", ret);
    };
    if (natEnabled_) {
        release(natEnabled_, NetsysController::GetInstance().DisableNat(IFACE_NAME, upstreamIface_));
    }
    if (interfaceForwarding_) {
        release(interfaceForwarding_,
                NetsysController::GetInstance().IpfwdRemoveInterfaceForward(IFACE_NAME, upstreamIface_));
    }
    if (forwardingEnabled_) {
        release(forwardingEnabled_, NetsysController::GetInstance().IpDisableForwarding(FORWARDING_REQUESTER));
    }
    if (error == NETSYS_SUCCESS) {
        upstreamIface_.clear();
        upstreamNetId_ = -1;
    }
    return error;
}

bool NearlinkIpShareController::Cleanup(bool publishIdle)
{
    {
        std::lock_guard lock(mutex_);
        ++generation_; // Discard already queued DHCP/link/network callbacks.
    }
    int32_t error = 0;
    NearlinkIpShareStatus idle;
    auto result = [&error](const char *resource, int32_t ret) {
        NETMGR_EXT_LOG_I("[NearlinkIpShare][Cleanup] resource=%{public}s code=%{public}d", resource, ret);
        if (ret != 0 && error == 0) {
            error = ret;
        }
        return ret == 0;
    };
    if (upstreamCallback_ != nullptr &&
        result("upstream callback", NetConnClient::GetInstance().UnregisterNetConnCallback(upstreamCallback_))) {
        upstreamCallback_ = nullptr;
    }
    if (netSupplierId_ != 0 &&
        result("supplier/routes/DNS", NetConnClient::GetInstance().UnregisterNetSupplier(netSupplierId_))) {
        netSupplierId_ = 0;
        supplierAvailable_ = false;
        appliedLink_ = NetLinkInfo{};
    }
    result("NAT/forwarding", CleanupUpstream());
    families_ = NearlinkFamilyNetwork{};
    pendingIpv4_ = NetLinkInfo{};
    leaseExpiry_ = pendingLeaseExpiry_ = std::chrono::steady_clock::time_point{};
    ipv4PublishPending_ = false;
    networkDirty_ = false;
    ipv6Addresses_ = DhcpL3Ipv6Snapshot{};
    maintenancePending_ = false;
    ++networkRevision_;
    validationInFlight_ = false;
    if (dnsProxyStarted_ && result("DNS proxy", NetsysController::GetInstance().StopDnsProxyListen())) {
        dnsProxyStarted_ = false;
    }
    if (dhcpClientStarted_ && result("DHCP client", StopDhcpClient(IFACE_NAME, dualStack_, true))) {
        dhcpClientStarted_ = false;
    }
    if (dhcpServerStarted_ && result("DHCP server", StopDhcpServer(IFACE_NAME))) {
        dhcpServerStarted_ = false;
    }
    if (result("IPv6 runtime", ipv6Runtime_.Cleanup() ? 0 : NETMANAGER_EXT_ERR_OPERATION_FAILED)) {
        ipv6Prepared_ = false;
    }
    if (localRouteAdded_ &&
        result("local route", NetsysController::GetInstance().NetworkRemoveRoute(IP_SHARE_LOCAL_NET_ID, IFACE_NAME,
                                                                                 LOCAL_SUBNET, DIRECT_NEXT_HOP))) {
        localRouteAdded_ = false;
    }
    if (!localRouteAdded_ && localInterfaceAdded_ &&
        result("local interface",
               NetsysController::GetInstance().NetworkRemoveInterface(IP_SHARE_LOCAL_NET_ID, IFACE_NAME))) {
        localInterfaceAdded_ = false;
    }
    if (addressConfigured_) {
        int32_t ret = NetsysController::GetInstance().DelInterfaceAddress(
            IFACE_NAME, configuration_.GetNearlinkIpv4Addr(), PREFIX_LENGTH);
        if (ret != 0) {
            // The interface can disappear before a cleanup retry. Do not retain
            // ownership of an address that the kernel no longer has.
            if (ret == -ENODEV || ret == -EADDRNOTAVAIL) {
                ret = 0;
            } else {
                if (IsGatewayAddressAbsent(&NetsysController::GetInterfaceConfig)) {
                    ret = 0;
                }
            }
        }
        if (result("address", ret)) {
            addressConfigured_ = false;
        }
    }
    // Always close the channel after DHCP stop/failure: this revokes the IPv4
    // gate even when DHCP cleanup fails. Retain failed resource ownership.
    if (nearlinkStarted_ && result("NearLink", OHOS::Nearlink::NearlinkIpShareClient::GetInstance().Stop())) {
        nearlinkStarted_ = false;
    }
    {
        std::lock_guard lock(mutex_);
        if (error == 0 && gatewayReserved_) {
            NetworkShareAdmission::GetInstance().ReleaseNearlink();
            gatewayReserved_ = false;
        }
        stopRequested_ = false;
        if (error == 0 && publishIdle) {
            idle.generation = status_.generation;
            idle.sequence = status_.sequence + 1;
            idle.requestedMode = status_.requestedMode;
            status_ = idle;
        }
        status_.ipv4Address.clear();
        status_.hasUpstream = false;
    }
    if (error != 0) {
        Publish(NearlinkIpShareState::ERROR, "CLEANUP", error);
        return false;
    }
    if (publishIdle) {
        // Publish the completed snapshot without overwriting a newly reserved start.
        NetworkShareTracker::GetInstance().SendNearlinkStateChange(idle);
    }
    return true;
}

void NearlinkIpShareController::Fail(const std::string &stage, int32_t code)
{
    NearlinkIpShareStatus failedStatus;
    {
        std::lock_guard lock(mutex_);
        failedStatus = status_;
        status_.state = NearlinkIpShareState::ERROR;
    }
    NETMGR_EXT_LOG_E("[NearlinkIpShare][Failure] role=%{public}d errorStage=%{public}s code=%{public}d",
                     static_cast<int32_t>(failedStatus.role), stage.c_str(), code);
    if (!Cleanup(false)) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        status_.role = failedStatus.role;
        status_.peerAddress = failedStatus.peerAddress;
        status_.ifaceName = failedStatus.ifaceName;
    }
    Publish(NearlinkIpShareState::ERROR, stage, code);
}

void NearlinkIpShareController::Publish(NearlinkIpShareState state, const std::string &errorStage, int32_t errorCode)
{
    NearlinkIpShareStatus snapshot;
    {
        std::lock_guard lock(mutex_);
        auto previous = status_.state;
        status_.state = state;
        status_.errorStage = errorStage;
        status_.errorCode = errorCode;
        ++status_.sequence;
        snapshot = status_;
        NETMGR_EXT_LOG_I("[NearlinkIpShare][Families] generation=%{public}llu sequence=%{public}llu netId=%{public}d "
                         "ipv4=%{public}d validation4=%{public}d ipv6=%{public}d validation6=%{public}d",
                         static_cast<unsigned long long>(status_.generation),
                         static_cast<unsigned long long>(status_.sequence), status_.netId,
                         status_.ipv4.configurationAvailable, status_.ipv4.validation,
                         status_.ipv6.configurationAvailable, status_.ipv6.validation);
        NETMGR_EXT_LOG_I("[NearlinkIpShare][State] role=%{public}d %{public}d->%{public}d peer=%{public}s "
                         "errorStage=%{public}s code=%{public}d",
                         static_cast<int32_t>(status_.role), static_cast<int32_t>(previous),
                         static_cast<int32_t>(state), MaskPeer(status_.peerAddress).c_str(), errorStage.c_str(),
                         errorCode);
    }
    NetworkShareTracker::GetInstance().SendNearlinkStateChange(snapshot);
}
} // namespace OHOS::NetManagerStandard
