/* Copyright (c) 2026 Huawei Device Co., Ltd. Licensed under the Apache License, Version 2.0. */
#include "nearlink_ipshare_controller.h"

#include <new>

#include "net_manager_constants.h"
#include "netmgr_ext_log_wrapper.h"

namespace OHOS::NetManagerStandard {
void NearlinkIpShareController::NotifyGatewayUpstream(PeerAddressContext &peer, bool ipv4)
{
    if (peer.releasing) {
        return;
    }
    if (!peer.upstreamSignal) {
        peer.upstreamSignal.reset(new (std::nothrow) NearlinkUpstreamSignal());
    }
    if (!peer.upstreamSignal) {
        return;
    }
    NearlinkUpstreamSignal::Link link;
    link.iface = peer.iface;
    link.ifindex = peer.ifindex;
    link.netId = 99; // Existing local network owns every gateway sleipN subnet route.
    link.localIpv4 = peer.addressAdded ? peer.addresses.gateway : "";
    link.ipv6 = peer.mode == 3 && peer.ipv6Prepared;
    uint8_t available =
        (ipv4 ? NearlinkUpstreamSignal::IPV4 : 0) | (peer.ipv6Routed ? NearlinkUpstreamSignal::IPV6 : 0);
    peer.upstreamSignal->Gateway(link, available);
}

bool NearlinkIpShareController::RemoteFamilyBlocked(bool ipv6) const
{
    return gatewayUpstream_.known &&
           !(gatewayUpstream_.available & (ipv6 ? NearlinkUpstreamSignal::IPV6 : NearlinkUpstreamSignal::IPV4));
}

void NearlinkIpShareController::ApplyRemoteFamilyStatus(NearlinkIpShareFamilyStatus &family, bool ipv6)
{
    if (!RemoteFamilyBlocked(ipv6)) {
        if (family.hasError && family.error.stage == "UPSTREAM") {
            family.hasError = false;
            family.error = {};
        }
        return;
    }
    family.externalAvailable = false;
    family.validation = family.configurationAvailable ? 3 : 0;
    if (family.configurationAvailable && (!family.hasError || family.error.stage == "UPSTREAM")) {
        family.hasError = true;
        family.error.plane = 4;
        family.error.family = ipv6 ? 2 : 1;
        family.error.stage = "UPSTREAM";
        family.error.code = NETMANAGER_EXT_ERR_OPERATION_FAILED;
        family.error.retryable = true;
    }
}

bool NearlinkIpShareController::PollTerminalUpstream()
{
    NearlinkUpstreamSignal::Link link;
    link.iface = "sleip0";
    link.ifindex = interfaceIndex_;
    link.ipv6 = dualStack_ && ipv6Prepared_;
    if (!families_.ipv4.netAddrList_.empty()) {
        link.localIpv4 = families_.ipv4.netAddrList_.front().address_;
    }
    for (const auto &route : families_.ipv4.routeList_) {
        if (route.destination_.prefixlen_ == 0) {
            link.gatewayIpv4 = route.gateway_.address_;
        }
    }
    {
        std::lock_guard lock(mutex_);
        link.netId = status_.netId;
    }
    const auto next = upstreamSignal_.Terminal(link);
    {
        std::lock_guard lock(mutex_);
        if (next.known == gatewayUpstream_.known && next.available == gatewayUpstream_.available && !next.changed) {
            return false;
        }
        for (size_t i = 0; i < validation_.size(); ++i) {
            uint8_t bit = i == 0 ? NearlinkUpstreamSignal::IPV4 : NearlinkUpstreamSignal::IPV6;
            bool wasBlocked = gatewayUpstream_.known && !(gatewayUpstream_.available & bit);
            bool blocked = next.known && !(next.available & bit);
            if (wasBlocked == blocked && !(next.changed & bit)) {
                continue;
            }
            // Invalidate only this family's old completion. Keep one in-flight probe
            // until it finishes; its revision guard rejects any late PASS.
            ++validation_[i].revision;
            validation_[i].next = {};
            auto &family = i == 0 ? status_.ipv4 : status_.ipv6;
            family.externalAvailable = false;
            family.validation = 0;
        }
        gatewayUpstream_ = next;
    }
    NETMGR_EXT_LOG_I("[NearlinkIpShare][RemoteUpstream] known=%{public}d ipv4=%{public}d ipv6=%{public}d", next.known,
                     !!(next.available & NearlinkUpstreamSignal::IPV4),
                     !!(next.available & NearlinkUpstreamSignal::IPV6));
    RequestFamilyValidation();
    return true;
}
} // namespace OHOS::NetManagerStandard
