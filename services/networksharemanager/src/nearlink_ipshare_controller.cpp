/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */
#include "nearlink_ipshare_client.h"

#include "nearlink_ipshare_controller.h"

#include <cctype>
#include <future>
#include <set>

#include "dhcp_c_api.h"
#include "dhcp_result_event.h"
#include "net_conn_client.h"
#include "net_manager_constants.h"
#include "netmgr_ext_log_wrapper.h"
#include "netsys_controller.h"
#include "networkshare_tracker.h"
#include "securec.h"

namespace OHOS::NetManagerStandard {
namespace {
constexpr const char *IFACE_NAME = "sleip0";
constexpr const char *SUBNET_MASK = "255.255.255.0";
constexpr const char *FORWARDING_REQUESTER = "NearlinkIpShare";
constexpr int32_t PREFIX_LENGTH = 24;
constexpr int32_t DHCP_IPV4 = 0;
constexpr int32_t NETWORK_SCORE = 60;

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

void DhcpSuccessCallback(int status, const char *ifname, DhcpResult *result)
{
    if (ifname != nullptr && result != nullptr) {
        NearlinkIpShareController::GetInstance()->OnDhcpSuccess(status, ifname, *result);
    }
}

void DhcpFailureCallback(int status, const char *ifname, const char *reason)
{
    NearlinkIpShareController::GetInstance()->OnDhcpFailure(status, ifname == nullptr ? "" : ifname,
        reason == nullptr ? "" : reason);
}
} // namespace

std::shared_ptr<NearlinkIpShareController> NearlinkIpShareController::GetInstance()
{
    static auto instance = std::shared_ptr<NearlinkIpShareController>(new NearlinkIpShareController());
    return instance;
}

bool NearlinkIpShareController::Init()
{
    std::lock_guard lock(mutex_);
    if (initialized_) {
        return true;
    }
    nearlinkObserver_ = std::make_shared<NearlinkObserver>(std::weak_ptr<NearlinkIpShareController>(shared_from_this()));
    int32_t ret = OHOS::Nearlink::NearlinkIpShareClient::GetInstance().RegisterObserver(nearlinkObserver_);
    if (ret != 0) {
        NETMGR_EXT_LOG_E("[NearlinkIpShare][Init] observer registration failed code=%{public}d", ret);
        nearlinkObserver_.reset();
        return false;
    }
    initialized_ = true;
    NETMGR_EXT_LOG_I("[NearlinkIpShare][Init] controller ready");
    return true;
}

void NearlinkIpShareController::Uninit()
{
    Cleanup();
    OHOS::Nearlink::NearlinkIpShareClient::GetInstance().UnregisterObserver();
    std::lock_guard lock(mutex_);
    initialized_ = false;
    nearlinkObserver_.reset();
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
    std::array<uint8_t, 6> bytes {};
    if (!ParsePeerAddress(peerAddress, bytes)) {
        NETMGR_EXT_LOG_E("[NearlinkIpShare][Support] invalid peer address");
        return NETMANAGER_EXT_ERR_PARAMETER_ERROR;
    }
    auto result = std::make_shared<std::promise<std::pair<int32_t, bool>>>();
    auto future = result->get_future();
    if (!NetworkShareTracker::GetInstance().SubmitNearlinkTask([peerAddress, result]() {
        bool taskSupported = false;
        int32_t code = OHOS::Nearlink::NearlinkIpShareClient::GetInstance().IsPeerSupported(
            peerAddress, taskSupported);
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

int32_t NearlinkIpShareController::StartGateway(const std::string &peerAddress)
{
    return Start(NearlinkIpShareRole::GATEWAY, peerAddress);
}

int32_t NearlinkIpShareController::StartTerminal(const std::string &gatewayAddress)
{
    return Start(NearlinkIpShareRole::TERMINAL, gatewayAddress);
}

int32_t NearlinkIpShareController::Start(NearlinkIpShareRole role, const std::string &peerAddress)
{
    std::array<uint8_t, 6> bytes {};
    if (!ParsePeerAddress(peerAddress, bytes)) {
        NETMGR_EXT_LOG_E("[NearlinkIpShare][Start] role=%{public}d invalid peer address",
            static_cast<int32_t>(role));
        return NETMANAGER_EXT_ERR_PARAMETER_ERROR;
    }
    {
        std::lock_guard lock(mutex_);
        if (!initialized_) {
            NETMGR_EXT_LOG_E("[NearlinkIpShare][Start] role=%{public}d controller unavailable",
                static_cast<int32_t>(role));
            return NETMANAGER_EXT_ERR_OPERATION_FAILED;
        }
        if (status_.role != NearlinkIpShareRole::NONE) {
            if (status_.role == role && status_.peerAddress == peerAddress) {
                auto snapshot = status_;
                NetworkShareTracker::GetInstance().SubmitNearlinkTask([snapshot]() {
                    NetworkShareTracker::GetInstance().SendNearlinkStateChange(snapshot);
                });
                return NETMANAGER_EXT_SUCCESS;
            }
            NETMGR_EXT_LOG_E("[NearlinkIpShare][Start] busy currentRole=%{public}d requestedRole=%{public}d",
                static_cast<int32_t>(status_.role), static_cast<int32_t>(role));
            return NETMANAGER_EXT_ERR_OPERATION_FAILED;
        }
        status_.role = role;
        status_.peerAddress = peerAddress;
        status_.ifaceName.clear();
        status_.ipv4Address.clear();
        status_.hasUpstream = false;
        status_.errorStage.clear();
        status_.errorCode = 0;
    }
    auto self = shared_from_this();
    if (!NetworkShareTracker::GetInstance().SubmitNearlinkTask([self, role, peerAddress]() {
        self->Publish(NearlinkIpShareState::STARTING);
        int32_t ret = role == NearlinkIpShareRole::GATEWAY ?
            OHOS::Nearlink::NearlinkIpShareClient::GetInstance().StartGateway(peerAddress) :
            OHOS::Nearlink::NearlinkIpShareClient::GetInstance().StartTerminal(peerAddress);
        NETMGR_EXT_LOG_I("[NearlinkIpShare][Start] role=%{public}d peer=%{public}s accepted=%{public}d",
            static_cast<int32_t>(role), MaskPeer(peerAddress).c_str(), ret);
        if (ret != 0) {
            self->Fail("LINK", ret);
        }
    })) {
        std::lock_guard lock(mutex_);
        status_ = NearlinkIpShareStatus {};
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
    {
        std::lock_guard lock(mutex_);
        if (status_.role == NearlinkIpShareRole::NONE) {
            return NETMANAGER_EXT_SUCCESS;
        }
        if (status_.state == NearlinkIpShareState::STOPPING) {
            return NETMANAGER_EXT_SUCCESS;
        }
        if (status_.role != expectedRole) {
            NETMGR_EXT_LOG_E("[NearlinkIpShare][Stop] role mismatch current=%{public}d requested=%{public}d",
                static_cast<int32_t>(status_.role), static_cast<int32_t>(expectedRole));
            return NETMANAGER_EXT_ERR_PARAMETER_ERROR;
        }
    }
    auto self = shared_from_this();
    return NetworkShareTracker::GetInstance().SubmitNearlinkTask([self]() {
        self->Publish(NearlinkIpShareState::STOPPING);
        self->Cleanup();
    }) ?
        NETMANAGER_EXT_SUCCESS : NETMANAGER_EXT_ERR_OPERATION_FAILED;
}

int32_t NearlinkIpShareController::GetStatus(NearlinkIpShareStatus &status) const
{
    std::lock_guard lock(mutex_);
    status = status_;
    return NETMANAGER_EXT_SUCCESS;
}

void NearlinkIpShareController::ReplayStatus(const sptr<ISharingEventCallback> &callback) const
{
    if (callback == nullptr) {
        return;
    }
    NearlinkIpShareStatus status;
    GetStatus(status);
    callback->OnNearlinkIpShareStateChanged(status);
}

void NearlinkIpShareController::OnNearlinkStatus(const OHOS::Nearlink::NearlinkIpShareStatus &status)
{
    auto self = shared_from_this();
    if (!NetworkShareTracker::GetInstance().SubmitNearlinkTask([self, status]() { self->HandleNearlinkStatus(status); })) {
        Fail("INTERNAL", NETMANAGER_EXT_ERR_OPERATION_FAILED);
    }
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
        status_.ifaceName = status.ifaceName;
    }
    NETMGR_EXT_LOG_I("[NearlinkIpShare][NearLink] role=%{public}d state=%{public}d peer=%{public}s iface=%{public}s",
        static_cast<int32_t>(role), static_cast<int32_t>(status.state), MaskPeer(status.peerAddress).c_str(),
        status.ifaceName.c_str());
    if (status.state == OHOS::Nearlink::NearlinkIpShareState::ERROR) {
        Fail(status.errorStage.empty() ? "LINK" : status.errorStage, status.errorCode);
        return;
    }
    if (role == NearlinkIpShareRole::GATEWAY &&
        status.state == OHOS::Nearlink::NearlinkIpShareState::IFACE_READY) {
        ConfigureGateway();
        return;
    }
    if (role == NearlinkIpShareRole::TERMINAL &&
        status.state == OHOS::Nearlink::NearlinkIpShareState::CHANNEL_READY) {
        std::string peerAddress;
        {
            std::lock_guard lock(mutex_);
            peerAddress = status_.peerAddress;
        }
        StartTerminalDhcp(peerAddress);
        return;
    }
    auto state = static_cast<NearlinkIpShareState>(static_cast<int32_t>(status.state));
    if (state <= NearlinkIpShareState::CHANNEL_READY) {
        Publish(state);
    }
}

void NearlinkIpShareController::ConfigureGateway()
{
    if (addressConfigured_) {
        return;
    }
    Publish(NearlinkIpShareState::CONFIGURING);
    const std::string &gatewayAddress = configuration_.GetNearlinkIpv4Addr();
    const std::string &poolStart = configuration_.GetNearlinkDhcpStart();
    const std::string &poolEnd = configuration_.GetNearlinkDhcpEnd();
    if (gatewayAddress.empty() || poolStart.empty() || poolEnd.empty()) {
        Fail("INTERNAL", NETMANAGER_EXT_ERR_OPERATION_FAILED);
        return;
    }
    int32_t ret = NetsysController::GetInstance().AddInterfaceAddress(IFACE_NAME, gatewayAddress, PREFIX_LENGTH);
    if (ret != NETSYS_SUCCESS) {
        Fail("TUN", ret);
        return;
    }
    addressConfigured_ = true;
    NETMGR_EXT_LOG_I("[NearlinkIpShare][Gateway] interface=%{public}s address configured prefix=%{public}d",
        IFACE_NAME, PREFIX_LENGTH);

    DhcpRange range {};
    range.iptype = DHCP_IPV4;
    range.leaseHours = 6;
    if (strcpy_s(range.strTagName, sizeof(range.strTagName), IFACE_NAME) != EOK ||
        strcpy_s(range.strStartip, sizeof(range.strStartip), poolStart.c_str()) != EOK ||
        strcpy_s(range.strEndip, sizeof(range.strEndip), poolEnd.c_str()) != EOK ||
        strcpy_s(range.strSubnet, sizeof(range.strSubnet), SUBNET_MASK) != EOK) {
        Fail("DHCP", NETMANAGER_EXT_ERR_OPERATION_FAILED);
        return;
    }
    ret = SetDhcpRange(IFACE_NAME, &range);
    if (ret != DHCP_SUCCESS || (ret = StartDhcpServer(IFACE_NAME)) != DHCP_SUCCESS) {
        Fail("DHCP", ret);
        return;
    }
    dhcpServerStarted_ = true;
    NETMGR_EXT_LOG_I("[NearlinkIpShare][Gateway] DHCP interface=%{public}s pool configured and server started",
        IFACE_NAME);

    ret = NetsysController::GetInstance().StartDnsProxyListen();
    if (ret != NETSYS_SUCCESS) {
        Fail("DNS", ret);
        return;
    }
    dnsProxyStarted_ = true;

    NetHandle upstream;
    sptr<NetLinkInfo> linkInfo = sptr<NetLinkInfo>::MakeSptr();
    ret = NetConnClient::GetInstance().GetDefaultNet(upstream);
    if (ret != NETMANAGER_SUCCESS || upstream.GetNetId() < 0 || linkInfo == nullptr ||
        NetConnClient::GetInstance().GetConnectionProperties(upstream, *linkInfo) != NETMANAGER_SUCCESS ||
        linkInfo->ifaceName_.empty()) {
        NETMGR_EXT_LOG_I("[NearlinkIpShare][Gateway] no upstream; local DHCP remains available");
        {
            std::lock_guard lock(mutex_);
            status_.ifaceName = IFACE_NAME;
            status_.ipv4Address = gatewayAddress;
            status_.hasUpstream = false;
        }
        Publish(NearlinkIpShareState::SERVING_NO_UPSTREAM);
        return;
    }

    upstreamIface_ = linkInfo->ifaceName_;
    ret = NetsysController::GetInstance().ShareDnsSet(upstream.GetNetId());
    if (ret != NETSYS_SUCCESS) {
        Fail("DNS", ret);
        return;
    }
    ret = NetsysController::GetInstance().IpEnableForwarding(FORWARDING_REQUESTER);
    if (ret != NETSYS_SUCCESS) {
        Fail("UPSTREAM", ret);
        return;
    }
    forwardingEnabled_ = true;
    ret = NetsysController::GetInstance().IpfwdAddInterfaceForward(IFACE_NAME, upstreamIface_);
    if (ret != NETSYS_SUCCESS) {
        Fail("NAT", ret);
        return;
    }
    interfaceForwarding_ = true;
    ret = NetsysController::GetInstance().EnableNat(IFACE_NAME, upstreamIface_);
    if (ret != NETSYS_SUCCESS) {
        Fail("NAT", ret);
        return;
    }
    natEnabled_ = true;
    {
        std::lock_guard lock(mutex_);
        status_.ifaceName = IFACE_NAME;
        status_.ipv4Address = gatewayAddress;
        status_.hasUpstream = true;
    }
    NETMGR_EXT_LOG_I("[NearlinkIpShare][Gateway] DNS/forwarding/NAT ready down=%{public}s up=%{public}s",
        IFACE_NAME, upstreamIface_.c_str());
    Publish(NearlinkIpShareState::SERVING);
}

void NearlinkIpShareController::StartTerminalDhcp(const std::string &peerAddress)
{
    if (dhcpClientStarted_) {
        return;
    }
    std::array<uint8_t, 6> clientKey {};
    if (!ParsePeerAddress(peerAddress, clientKey)) {
        Fail("DHCP", NETMANAGER_EXT_ERR_PARAMETER_ERROR);
        return;
    }
    Publish(NearlinkIpShareState::DHCP);
    static const ClientCallBack callback {DhcpSuccessCallback, DhcpFailureCallback};
    int32_t ret = RegisterDhcpClientCallBack(IFACE_NAME, &callback);
    if (ret != DHCP_SUCCESS) {
        Fail("DHCP", ret);
        return;
    }
    RouterConfig config {};
    if (strcpy_s(config.ifname, sizeof(config.ifname), IFACE_NAME) != EOK) {
        Fail("DHCP", NETMANAGER_EXT_ERR_OPERATION_FAILED);
        return;
    }
    config.bIpv6 = false;
    config.bIpv4 = true;
    config.prohibitUseCacheIp = true;
    config.linkMode = DHCP_LINK_MODE_L3_TUN;
    for (size_t i = 0; i < clientKey.size(); ++i) {
        config.clientKey[i] = clientKey[i];
    }
    ret = StartDhcpClient(config);
    if (ret != DHCP_SUCCESS) {
        Fail("DHCP", ret);
        return;
    }
    dhcpClientStarted_ = true;
    NETMGR_EXT_LOG_I("[NearlinkIpShare][Terminal] DHCP L3_TUN started interface=%{public}s peer=%{public}s",
        IFACE_NAME, MaskPeer(peerAddress).c_str());
}

void NearlinkIpShareController::OnDhcpSuccess(int32_t status, const std::string &iface, const DhcpResult &result)
{
    NETMGR_EXT_LOG_I("[NearlinkIpShare][DHCP] async success callback interface=%{public}s code=%{public}d",
        iface.c_str(), status);
    auto self = shared_from_this();
    NetworkShareTracker::GetInstance().SubmitNearlinkTask([self, status, iface, result]() {
        if (iface != IFACE_NAME || !result.isOptSuc) {
            self->Fail("DHCP", status);
            return;
        }
        self->ApplyTerminalNetwork(result);
    });
}

void NearlinkIpShareController::OnDhcpFailure(int32_t status, const std::string &iface, const std::string &reason)
{
    (void)reason;
    NETMGR_EXT_LOG_E("[NearlinkIpShare][DHCP] async failure callback interface=%{public}s code=%{public}d",
        iface.c_str(), status);
    auto self = shared_from_this();
    NetworkShareTracker::GetInstance().SubmitNearlinkTask([self, status, iface]() {
        if (iface == IFACE_NAME) {
            self->Fail("DHCP", status);
        }
    });
}

void NearlinkIpShareController::ApplyTerminalNetwork(const DhcpResult &result)
{
    std::set<NetCap> caps {NET_CAPABILITY_INTERNET, NET_CAPABILITY_NOT_VPN};
    int32_t ret = NetConnClient::GetInstance().RegisterNetSupplier(BEARER_BLUETOOTH, IFACE_NAME, caps, netSupplierId_);
    if (ret != NETMANAGER_SUCCESS) {
        Fail("ROUTE", ret);
        return;
    }
    sptr<NetLinkInfo> linkInfo = sptr<NetLinkInfo>::MakeSptr();
    if (linkInfo == nullptr) {
        Fail("INTERNAL", NETMANAGER_EXT_ERR_LOCAL_PTR_NULL);
        return;
    }
    linkInfo->ifaceName_ = IFACE_NAME;
    INetAddr address;
    address.type_ = INetAddr::IPV4;
    address.family_ = INetAddr::IPV4;
    address.address_ = result.strOptClientId;
    address.netMask_ = result.strOptSubnet;
    address.prefixlen_ = PREFIX_LENGTH;
    linkInfo->netAddrList_.push_back(address);

    Route route;
    route.iface_ = IFACE_NAME;
    route.destination_.type_ = INetAddr::IPV4;
    route.destination_.family_ = INetAddr::IPV4;
    route.destination_.address_ = "0.0.0.0";
    route.destination_.prefixlen_ = 0;
    route.gateway_.type_ = INetAddr::IPV4;
    route.gateway_.family_ = INetAddr::IPV4;
    route.gateway_.address_ = result.strOptRouter1;
    linkInfo->routeList_.push_back(route);
    for (const char *dnsAddress : {result.strOptDns1, result.strOptDns2}) {
        if (dnsAddress != nullptr && dnsAddress[0] != '\0') {
            INetAddr dns;
            dns.type_ = INetAddr::IPV4;
            dns.family_ = INetAddr::IPV4;
            dns.address_ = dnsAddress;
            linkInfo->dnsList_.push_back(dns);
        }
    }
    linkInfo->isUserDefinedDnsServer_ = true;
    ret = NetConnClient::GetInstance().UpdateNetLinkInfo(netSupplierId_, linkInfo);
    if (ret != NETMANAGER_SUCCESS) {
        Fail("ROUTE", ret);
        return;
    }
    sptr<NetSupplierInfo> supplierInfo = sptr<NetSupplierInfo>::MakeSptr();
    if (supplierInfo == nullptr) {
        Fail("INTERNAL", NETMANAGER_EXT_ERR_LOCAL_PTR_NULL);
        return;
    }
    supplierInfo->isAvailable_ = true;
    supplierInfo->score_ = NETWORK_SCORE;
    ret = NetConnClient::GetInstance().UpdateNetSupplierInfo(netSupplierId_, supplierInfo);
    if (ret != NETMANAGER_SUCCESS) {
        Fail("ROUTE", ret);
        return;
    }
    {
        std::lock_guard lock(mutex_);
        status_.ifaceName = IFACE_NAME;
        status_.ipv4Address = result.strOptClientId;
        status_.hasUpstream = true;
    }
    NETMGR_EXT_LOG_I("[NearlinkIpShare][Terminal] DHCP callback applied interface=%{public}s route=1 dns=%{public}zu "
        "validation=requested", IFACE_NAME, linkInfo->dnsList_.size());
    Publish(NearlinkIpShareState::ACTIVE);
}

void NearlinkIpShareController::Cleanup(bool publishIdle)
{
    NearlinkIpShareRole role;
    {
        std::lock_guard lock(mutex_);
        role = status_.role;
    }
    int32_t ret = 0;
    if (netSupplierId_ != 0) {
        sptr<NetSupplierInfo> supplierInfo = sptr<NetSupplierInfo>::MakeSptr();
        if (supplierInfo != nullptr) {
            supplierInfo->isAvailable_ = false;
            ret = NetConnClient::GetInstance().UpdateNetSupplierInfo(netSupplierId_, supplierInfo);
            NETMGR_EXT_LOG_I("[NearlinkIpShare][Cleanup] validation/network unavailable code=%{public}d", ret);
        }
        ret = NetConnClient::GetInstance().UnregisterNetSupplier(netSupplierId_);
        NETMGR_EXT_LOG_I("[NearlinkIpShare][Cleanup] route/DNS supplier removed code=%{public}d", ret);
        netSupplierId_ = 0;
    }
    if (natEnabled_) {
        ret = NetsysController::GetInstance().DisableNat(IFACE_NAME, upstreamIface_);
        NETMGR_EXT_LOG_I("[NearlinkIpShare][Cleanup] NAT removed code=%{public}d", ret);
        natEnabled_ = false;
    }
    if (interfaceForwarding_) {
        ret = NetsysController::GetInstance().IpfwdRemoveInterfaceForward(IFACE_NAME, upstreamIface_);
        NETMGR_EXT_LOG_I("[NearlinkIpShare][Cleanup] interface forwarding removed code=%{public}d", ret);
        interfaceForwarding_ = false;
    }
    if (forwardingEnabled_) {
        ret = NetsysController::GetInstance().IpDisableForwarding(FORWARDING_REQUESTER);
        NETMGR_EXT_LOG_I("[NearlinkIpShare][Cleanup] forwarding disabled code=%{public}d", ret);
        forwardingEnabled_ = false;
    }
    if (dnsProxyStarted_) {
        ret = NetsysController::GetInstance().StopDnsProxyListen();
        NETMGR_EXT_LOG_I("[NearlinkIpShare][Cleanup] DNS proxy stopped code=%{public}d", ret);
        dnsProxyStarted_ = false;
    }
    if (dhcpClientStarted_) {
        ret = StopDhcpClient(IFACE_NAME, false, true);
        NETMGR_EXT_LOG_I("[NearlinkIpShare][Cleanup] DHCP client stopped code=%{public}d", ret);
        dhcpClientStarted_ = false;
    }
    if (dhcpServerStarted_) {
        ret = StopDhcpServer(IFACE_NAME);
        NETMGR_EXT_LOG_I("[NearlinkIpShare][Cleanup] DHCP server stopped code=%{public}d", ret);
        dhcpServerStarted_ = false;
    }
    if (addressConfigured_) {
        ret = NetsysController::GetInstance().DelInterfaceAddress(
            IFACE_NAME, configuration_.GetNearlinkIpv4Addr(), PREFIX_LENGTH);
        NETMGR_EXT_LOG_I("[NearlinkIpShare][Cleanup] interface address removed code=%{public}d", ret);
        addressConfigured_ = false;
    }
    upstreamIface_.clear();
    ret = OHOS::Nearlink::NearlinkIpShareClient::GetInstance().Stop();
    NETMGR_EXT_LOG_I("[NearlinkIpShare][Cleanup] NearLink stopped role=%{public}d code=%{public}d",
        static_cast<int32_t>(role), ret);
    {
        std::lock_guard lock(mutex_);
        status_ = NearlinkIpShareStatus {};
    }
    if (publishIdle) {
        Publish(NearlinkIpShareState::IDLE);
    }
}

void NearlinkIpShareController::Fail(const std::string &stage, int32_t code)
{
    NearlinkIpShareStatus failedStatus;
    {
        std::lock_guard lock(mutex_);
        failedStatus = status_;
    }
    NETMGR_EXT_LOG_E("[NearlinkIpShare][Failure] role=%{public}d errorStage=%{public}s code=%{public}d",
        static_cast<int32_t>(failedStatus.role), stage.c_str(), code);
    Cleanup(false);
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
        snapshot = status_;
        NETMGR_EXT_LOG_I("[NearlinkIpShare][State] role=%{public}d %{public}d->%{public}d peer=%{public}s "
            "errorStage=%{public}s code=%{public}d", static_cast<int32_t>(status_.role),
            static_cast<int32_t>(previous), static_cast<int32_t>(state),
            MaskPeer(status_.peerAddress).c_str(), errorStage.c_str(), errorCode);
    }
    NetworkShareTracker::GetInstance().SendNearlinkStateChange(snapshot);
}
} // namespace OHOS::NetManagerStandard
