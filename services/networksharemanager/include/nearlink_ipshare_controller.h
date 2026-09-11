/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */
#ifndef NETMANAGER_EXT_NEARLINK_IPSHARE_CONTROLLER_H
#define NETMANAGER_EXT_NEARLINK_IPSHARE_CONTROLLER_H

#include <array>
#include <memory>
#include <mutex>
#include <string>

#include "dhcp_result_event.h"
#include "nearlink_ip_share_status.h"
#include "networkshare_configuration.h"

namespace OHOS::Nearlink {
class NearlinkIpShareObserver;
class NearlinkIpShareStatus;
}

namespace OHOS::NetManagerStandard {
class INearlinkIpShareEventCallback;
class INetConnCallback;

class NearlinkIpShareController final : public std::enable_shared_from_this<NearlinkIpShareController> {
public:
    static std::shared_ptr<NearlinkIpShareController> GetInstance();

    bool Init();
    void Uninit();
    int32_t IsSupported(const std::string &peerAddress, bool &supported);
    int32_t StartGateway(const std::string &peerAddress);
    int32_t StopGateway();
    int32_t StartTerminal(const std::string &gatewayAddress);
    int32_t StopTerminal();
    int32_t GetStatus(NearlinkIpShareStatus &status) const;
    void ReplayStatus(const sptr<INearlinkIpShareEventCallback> &callback) const;

    void OnNearlinkStatus(const OHOS::Nearlink::NearlinkIpShareStatus &status);
    void OnDhcpSuccess(int32_t status, const std::string &iface, const DhcpResult &result);
    void OnDhcpFailure(int32_t status, const std::string &iface, const std::string &reason);
    void OnUpstreamChanged();

private:
    NearlinkIpShareController() = default;
    int32_t Start(NearlinkIpShareRole role, const std::string &peerAddress);
    int32_t Stop(NearlinkIpShareRole expectedRole);
    void HandleNearlinkStatus(const OHOS::Nearlink::NearlinkIpShareStatus &status);
    void ConfigureGateway();
    void StartTerminalDhcp();
    void ConfigureUpstream();
    int32_t CleanupUpstream();
    bool IsCurrentSession(uint64_t generation) const;
    void ApplyTerminalNetwork(const DhcpResult &result);
    bool Cleanup(bool publishIdle = true);
    void Fail(const std::string &stage, int32_t code);
    void Publish(NearlinkIpShareState state, const std::string &errorStage = {}, int32_t errorCode = 0);
    static bool ParsePeerAddress(const std::string &address, std::array<uint8_t, 6> &bytes);
    static std::string MaskPeer(const std::string &address);

    mutable std::mutex mutex_;
    NearlinkIpShareStatus status_;
    std::shared_ptr<OHOS::Nearlink::NearlinkIpShareObserver> nearlinkObserver_;
    bool initialized_ {false};
    bool shuttingDown_ {false};
    bool stopRequested_ {false};
    uint64_t generation_ {0};
    bool nearlinkStarted_ {false};
    bool localInterfaceAdded_ {false};
    bool localRouteAdded_ {false};
    bool addressConfigured_ {false};
    bool dhcpServerStarted_ {false};
    bool dnsProxyStarted_ {false};
    bool forwardingEnabled_ {false};
    bool interfaceForwarding_ {false};
    bool natEnabled_ {false};
    bool dhcpClientStarted_ {false};
    uint32_t netSupplierId_ {0};
    sptr<INetConnCallback> upstreamCallback_;
    int32_t upstreamNetId_ {-1};
    std::string upstreamIface_;
    NetworkShareConfiguration configuration_;
};
} // namespace OHOS::NetManagerStandard
#endif // NETMANAGER_EXT_NEARLINK_IPSHARE_CONTROLLER_H
