/* Copyright (c) 2026 Huawei Device Co., Ltd. Licensed under the Apache License, Version 2.0. */
#include "nearlink_ipshare_controller.h"
#include <algorithm>
#include <cerrno>
#include "net_manager_constants.h"
#include "netmgr_ext_log_wrapper.h"
#include "netsys_controller.h"

namespace OHOS::NetManagerStandard {
int32_t NearlinkIpShareController::CleanupGatewayPeerUpstream(PeerAddressContext &peer)
{
    auto &netsys = NetsysController::GetInstance();
    int32_t error = 0;
    // Each successful release commits independently; a failed resource keeps its old upstream binding.
    if (peer.natEnabled || peer.natAttempted) {
        error = netsys.DisableNat(peer.iface, peer.upstreamIface);
        if (error == 0)
            peer.natEnabled = peer.natAttempted = false;
    }
    if (peer.interfaceForwarding || peer.forwardAttempted) {
        int32_t ret = netsys.IpfwdRemoveInterfaceForward(peer.iface, peer.upstreamIface);
        if (ret == 0)
            peer.interfaceForwarding = peer.forwardAttempted = false;
        else if (error == 0)
            error = ret;
    }
    peer.ipv6Routed = false;
    if (error == 0) {
        peer.upstreamIface.clear();
        peer.upstreamNetId = -1;
    }
    peer.upstreamError = error;
    NETMGR_EXT_LOG_I("[NearlinkIpShare][PeerUpstreamCleanup] slot=%{public}u generation=%{public}llu "
                     "iface=%{public}s nat=%{public}d forward=%{public}d code=%{public}d",
                     peer.slot, static_cast<unsigned long long>(peer.generation), peer.iface.c_str(), peer.natEnabled,
                     peer.interfaceForwarding, error);
    return error;
}

void NearlinkIpShareController::ConfigureGatewayPeerUpstreams(const NetLinkInfo *upstream, int32_t netId)
{
    auto &netsys = NetsysController::GetInstance();
    // Forwarding/isolation and DNS belong to G, including its zero-peer listening state.
    if (!forwardingEnabled_) {
        forwardingAttempted_ = true;
        forwardingEnabled_ = netsys.IpEnableForwarding("NearlinkIpShare") == 0;
        if (forwardingEnabled_)
            forwardingAttempted_ = false;
    }
    if (!dnsProxyStarted_)
        dnsProxyStarted_ = netsys.StartDnsProxyListen() == 0;
    bool present =
        upstream && netId >= 0 && !upstream->ifaceName_.empty() && upstream->ifaceName_.compare(0, 5, "sleip") != 0;
    int32_t dnsRet = dnsProxyStarted_ ? netsys.ShareDnsSet(present ? netId : 0) : -1;
    dnsUpstreamReady_ = present && dnsRet == 0;
    upstreamNetId_ = present ? netId : -1;
    upstreamIface_ = present ? upstream->ifaceName_ : "";
    bool hasV4 =
        present && std::any_of(upstream->routeList_.begin(), upstream->routeList_.end(), [](const auto &route) {
            return route.destination_.family_ == AF_INET && route.destination_.prefixlen_ == 0;
        });
    bool anyForward = false;
    for (auto &[slot, peer] : addressPeers_) {
        if (peer.releasing) {
            CleanupGatewayPeerUpstream(peer);
            continue;
        }
        bool usable = present && forwardingEnabled_ && (peer.dhcpStarted || peer.ipv6Prepared);
        if ((!usable || peer.upstreamIface != upstreamIface_ || peer.upstreamNetId != upstreamNetId_) &&
            CleanupGatewayPeerUpstream(peer) != 0)
            usable = false;
        int32_t ret = peer.upstreamError;
        if (usable) {
            peer.upstreamIface = upstreamIface_;
            peer.upstreamNetId = upstreamNetId_;
            ret = 0;
            if (!peer.interfaceForwarding) {
                peer.forwardAttempted = true;
                ret = netsys.IpfwdAddInterfaceForward(peer.iface, peer.upstreamIface);
                peer.interfaceForwarding = ret == 0;
                if (ret == 0)
                    peer.forwardAttempted = false;
            }
            // EnableNat owns both families. Losing IPv4 must not tear down a prepared
            // IPv6 peer's MASQUERADE; RA/default readiness is checked independently below.
            bool needsNat = (hasV4 && peer.ipv4Error == 0) || (peer.mode == 3 && peer.ipv6Prepared);
            if (peer.interfaceForwarding && needsNat && !peer.natEnabled) {
                peer.natAttempted = true;
                ret = netsys.EnableNat(peer.iface, peer.upstreamIface);
                peer.natEnabled = ret == 0;
                if (ret == 0)
                    peer.natAttempted = false;
            } else if (!needsNat && (peer.natEnabled || peer.natAttempted)) {
                ret = netsys.DisableNat(peer.iface, peer.upstreamIface);
                if (ret == 0)
                    peer.natEnabled = peer.natAttempted = false;
            }
        }
        peer.upstreamError = ret;
        bool forwarding = usable && peer.interfaceForwarding;
        anyForward = anyForward || forwarding;
        peer.ipv6Routed = false;
        if (peer.mode == 3 && peer.ipv6Prepared) {
            std::string prefix;
            bool provisioned = false;
            const auto &pool = configuration_.GetNearlinkIpv6RoutedPool();
            if (present) {
                if (pool.empty()) {
                    // Preserve P2 automatic prefix derivation, with a unique /64 for each stable slot.
                    prefix = NearlinkIpv6Runtime::DeriveGatewayPrefix(upstream, peer.slot);
                    provisioned = !prefix.empty();
                } else if (configuration_.GetNearlinkIpv6RoutedUpstream() == upstreamIface_) {
                    NearlinkPeerAddresses routed;
                    provisioned = NearlinkPeerAddresses::Allocate(configuration_.GetNearlinkIpv4Pool(), pool,
                                                                  maxTerminals_, peer.slot, routed, true);
                    if (provisioned)
                        prefix = routed.prefix;
                }
            }
            // Losing the uplink does not renumber the downstream logical link.
            // Automatic NAT66 keeps one source prefix for this peer session:
            // MASQUERADE uses the new selected uplink after every recovery.
            // Explicit routed pools still require their newly provisioned prefix.
            if (peer.ipv6.HasPrefix() &&
                (!provisioned || pool.empty())) {
                prefix = peer.ipv6.CurrentPrefix();
            }
            if (present)
                for (const auto &address : upstream->netAddrList_)
                    if (address.family_ == AF_INET6 &&
                        NearlinkPeerAddresses::Ipv6Conflicts(prefix, address.address_, address.prefixlen_))
                        provisioned = false;
            // A changed upstream must not graft one peer onto another peer's retiring logical link.
            for (const auto &[otherSlot, other] : addressPeers_)
                if (otherSlot != slot && other.ipv6.OwnsPrefix(prefix))
                    provisioned = false;
            if (!provisioned)
                prefix = peer.ipv6.HasPrefix() ? peer.ipv6.CurrentPrefix() : peer.addresses.prefix;
            peer.ipv6.SetGatewayPrefix(prefix.empty() ? peer.addresses.prefix : prefix);
            peer.ipv6Ready =
                peer.ipv6.Advertise(present ? upstream : nullptr, provisioned && forwarding && peer.natEnabled,
                                    dnsProxyStarted_);
            peer.ipv6Routed =
                provisioned && forwarding && peer.natEnabled && peer.ipv6Ready && peer.ipv6.HasDefaultRouter() &&
                dnsUpstreamReady_;
            auto now = std::chrono::steady_clock::now();
            if (peer.ipv6Ready) {
                peer.ipv6PendingSince = {};
                peer.ipv6Error = 0;
            } else {
                if (peer.ipv6PendingSince == std::chrono::steady_clock::time_point{})
                    peer.ipv6PendingSince = now;
                peer.ipv6Error =
                    now - peer.ipv6PendingSince >= std::chrono::seconds(10) ? NETMANAGER_EXT_ERR_OPERATION_FAILED : 0;
            }
        }
        NETMGR_EXT_LOG_I("[NearlinkIpShare][PeerUpstream] slot=%{public}u generation=%{public}llu "
                         "iface=%{public}s forward=%{public}d nat=%{public}d dns=%{public}d "
                         "nat6=%{public}d ipv6Routed=%{public}d code=%{public}d",
                         slot, static_cast<unsigned long long>(peer.generation), peer.iface.c_str(), forwarding,
                         usable && hasV4 && peer.natEnabled, dnsUpstreamReady_,
                         usable && peer.mode == 3 && peer.natEnabled, peer.ipv6Routed, ret);
    }
    {
        std::lock_guard lock(mutex_);
        status_.hasUpstream = present && forwardingEnabled_ && (addressPeers_.empty() || anyForward);
        // Gateway rules are configuration evidence. End-to-end validation remains terminal-side.
        status_.ipv4.externalAvailable = status_.ipv6.externalAvailable = false;
        if (!addressPeers_.empty()) {
            const auto &peer = addressPeers_.begin()->second;
            bool v4Ready = peer.ipv4Error == 0 && peer.interfaceForwarding && peer.natEnabled && hasV4;
            status_.ipv4.hasError = peer.ipv4Error != 0 || (present && (!v4Ready || dnsRet != 0));
            status_.ipv4.error = {};
            if (status_.ipv4.hasError) {
                status_.ipv4.error.plane = 4;
                status_.ipv4.error.family = 1;
                status_.ipv4.error.stage = peer.ipv4Error ? "DHCP" : (dnsRet ? "DNS" : "NAT");
                status_.ipv4.error.code =
                    peer.ipv4Error
                        ? peer.ipv4Error
                        : (dnsRet ? dnsRet
                                  : (peer.upstreamError ? peer.upstreamError : NETMANAGER_EXT_ERR_OPERATION_FAILED));
                status_.ipv4.error.retryable = true;
            }
            status_.ipv6.configurationAvailable = peer.ipv6Ready;
            status_.ipv6.phase = peer.mode == 3 ? (peer.ipv6Ready ? 2 : (peer.ipv6Error ? 3 : 1)) : 0;
            // No routed default: explicit local-only/limited IPv6, without destroying IPv4.
            status_.ipv6.hasError = peer.mode == 3 && !peer.ipv6Routed;
            status_.ipv6.error = {};
            if (status_.ipv6.hasError) {
                status_.ipv6.error.plane = 4;
                status_.ipv6.error.family = 2;
                status_.ipv6.error.stage = present && peer.interfaceForwarding && !peer.natEnabled ? "NAT" : "PREFIX";
                status_.ipv6.error.code = peer.upstreamError ? peer.upstreamError : NETMANAGER_EXT_ERR_OPERATION_FAILED;
                status_.ipv6.error.retryable = true;
            }
        }
    }
}
} // namespace OHOS::NetManagerStandard
