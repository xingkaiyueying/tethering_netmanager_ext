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
#ifndef NETMANAGER_EXT_NEARLINK_IPSHARE_CONTROLLER_H
#define NETMANAGER_EXT_NEARLINK_IPSHARE_CONTROLLER_H

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#include "dhcp_result_event.h"
#include "dhcp_l3_ipv6.h"
#include "nearlink_family_network.h"
#include "nearlink_ipv6_runtime.h"
#include "nearlink_ip_share_status.h"
#include "networkshare_configuration.h"

namespace OHOS::Nearlink {
class NearlinkIpShareObserver;
class NearlinkIpShareStatus;
} // namespace OHOS::Nearlink

namespace OHOS::NetManagerStandard {
class INearlinkIpShareEventCallback;
class INetConnCallback;

class NearlinkIpShareController final : public std::enable_shared_from_this<NearlinkIpShareController> {
public:
    static std::shared_ptr<NearlinkIpShareController> GetInstance();

    int32_t QueryCapabilities(const std::string &peer, NearlinkIpShareCapabilities &capabilities);
    bool Init();
    void Uninit();
    int32_t IsSupported(const std::string &peerAddress, bool &supported);
    int32_t StartGateway(const std::string &peerAddress, int32_t mode = 1);
    int32_t StopGateway();
    int32_t StartTerminal(const std::string &gatewayAddress, int32_t mode = 1);
    int32_t StopTerminal();
    int32_t GetStatus(NearlinkIpShareStatus &status) const;
    void ReplayStatus(const sptr<INearlinkIpShareEventCallback> &callback) const;

    void OnNearlinkStatus(const OHOS::Nearlink::NearlinkIpShareStatus &status);
    void OnDhcpSuccess(int32_t status, const std::string &iface, const DhcpResult &result, uint64_t session = 0);
    void OnDhcpFailure(int32_t status, const std::string &iface, const std::string &reason, uint64_t session = 0);
    void OnUpstreamChanged();
    void OnIpv6Addresses(const std::string &iface, const DhcpL3Ipv6Snapshot &snapshot, uint64_t session = 0);

private:
    NearlinkIpShareController() = default;
    int32_t Start(NearlinkIpShareRole role, const std::string &peerAddress, int32_t mode);
    int32_t Stop(NearlinkIpShareRole expectedRole);
    void HandleNearlinkStatus(const OHOS::Nearlink::NearlinkIpShareStatus &status);
    void ConfigureGateway();
    int32_t ConfigureGatewayIpv4();
    void StartTerminalDhcp();
    void ConfigureUpstream();
    int32_t CleanupUpstream();
    bool IsCurrentSession(uint64_t generation) const;
    void ApplyTerminalNetwork(const DhcpResult &result);
    void RetryTerminalNetwork();
    void ApplyIpv6Network(const DhcpResult &result);
    bool PublishTerminalNetwork();
    void FamilyFailure(bool ipv6, const std::string &stage, int32_t code, bool withdraw = true);
    void ScheduleMaintenance(uint64_t generation);
    void RefreshFamilyStatus();
    void ValidateFamilies();
    uint64_t networkRevision_{0};
    bool validationInFlight_{false};
    std::chrono::steady_clock::time_point nextValidation_{};
    bool Cleanup(bool publishIdle = true);
    void Fail(const std::string &stage, int32_t code);
    void Publish(NearlinkIpShareState state, const std::string &errorStage = {}, int32_t errorCode = 0);
    static bool ParsePeerAddress(const std::string &address, std::array<uint8_t, 6> &bytes);
    static std::string MaskPeer(const std::string &address);

    // Serialize registration with teardown without holding the state lock across IPC callbacks.
    std::mutex initMutex_;
    mutable std::mutex mutex_;
    NearlinkIpShareStatus status_;
    std::shared_ptr<OHOS::Nearlink::NearlinkIpShareObserver> nearlinkObserver_;
    bool initialized_{false};
    bool shuttingDown_{false};
    bool stopRequested_{false};
    std::atomic<uint64_t> generation_{0};
    bool gatewayReserved_{false};
    bool nearlinkStarted_{false};
    bool localInterfaceAdded_{false};
    bool localRouteAdded_{false};
    bool addressConfigured_{false};
    bool dhcpServerStarted_{false};
    bool dnsProxyStarted_{false};
    bool dnsUpstreamReady_{false};
    bool forwardingEnabled_{false};
    bool interfaceForwarding_{false};
    bool natEnabled_{false};
    bool dhcpClientStarted_{false};
    std::chrono::steady_clock::time_point leaseExpiry_{};
    uint32_t netSupplierId_{0};
    sptr<INetConnCallback> upstreamCallback_;
    int32_t upstreamNetId_{-1};
    std::string upstreamIface_;
    NetworkShareConfiguration configuration_;
    NearlinkFamilyNetwork families_;
    NetLinkInfo appliedLink_;
    NetLinkInfo pendingIpv4_;
    std::chrono::steady_clock::time_point pendingLeaseExpiry_{};
    bool ipv4PublishPending_{false};
    bool networkDirty_{false};
    bool supplierAvailable_{false};
    NearlinkIpv6Runtime ipv6Runtime_;
    bool ipv6Prepared_{false};
    void ConfigureGatewayIpv6(const NetLinkInfo *upstream);
    DhcpL3Ipv6Snapshot ipv6Addresses_{};
    std::chrono::steady_clock::time_point ipv6Observed_{};
    DhcpResult ipv6Result_{};
    uint64_t linkGeneration_{0}, linkSequence_{0}, evidenceSequence_{0};
    uint32_t interfaceIndex_{0};
    bool dualStack_{false};
    bool channelReady_{false};
    bool retryIpv4_{false}, ipv6ClientStarted_{false};
    std::chrono::steady_clock::time_point nextDhcpRetry_{};
    std::array<uint8_t, 6> clientKey_{};
    void RetryTerminalDhcp();
    bool maintenancePending_{false};
};
} // namespace OHOS::NetManagerStandard
#endif // NETMANAGER_EXT_NEARLINK_IPSHARE_CONTROLLER_H
