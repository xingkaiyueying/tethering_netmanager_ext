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
#include <algorithm>
#include <cerrno>
#include <net/if.h>
#include <set>
#include "dhcp_c_api.h"
#include "nearlink_host.h"
#include "net_conn_client.h"
#include "net_manager_constants.h"
#include "netmgr_ext_log_wrapper.h"
#include "netsys_controller.h"
#include "securec.h"
namespace OHOS::NetManagerStandard {
namespace {
constexpr int32_t PEER_IP_SHARE_LOCAL_NET_ID = 99;
constexpr int32_t PEER_PREFIX_LENGTH = 24;
constexpr int32_t PEER_DHCP_IPV4 = 0;
constexpr const char *PEER_DIRECT_NEXT_HOP = "0.0.0.0";
constexpr const char *PEER_SUBNET_MASK = "255.255.255.0";
} // namespace
bool NearlinkIpShareController::CleanupGatewayPeer(PeerAddressContext &peer)
{
    if (CleanupGatewayPeerUpstream(peer) != 0)
        return false;
    bool ok = true;
    auto released = [&peer](const char *resource, int32_t ret, bool success) {
        NETMGR_EXT_LOG_I("[NearlinkIpShare][PeerCleanup] slot=%{public}u generation=%{public}llu "
                         "iface=%{public}s ifindex=%{public}u resource=%{public}s code=%{public}d released=%{public}d",
                         peer.slot, static_cast<unsigned long long>(peer.generation), peer.iface.c_str(),
                         peer.ifindex, resource, ret, success);
        return success;
    };
    if (peer.dhcpStarted) {
        int32_t ret = StopDhcpServer(peer.iface.c_str());
        if (released("DHCP", ret, ret == DHCP_SUCCESS))
            peer.dhcpStarted = false;
        else
            ok = false;
    }
    bool ipv6Released = peer.ipv6.Cleanup();
    if (released("IPv6", ipv6Released ? 0 : NETMANAGER_EXT_ERR_OPERATION_FAILED, ipv6Released))
        peer.ipv6Prepared = peer.ipv6Ready = false;
    else
        ok = false;
    auto &netsys = NetsysController::GetInstance();
    auto missing = [](int32_t result) {
        return result == 0 || result == -ENODEV || result == -ESRCH || result == -EADDRNOTAVAIL;
    };
    // An interface name may have been reused. Old addresses are never deleted from the new ifindex.
    bool sameInterface = if_nametoindex(peer.iface.c_str()) == peer.ifindex;
    if (peer.routeAdded) {
        int32_t ret = netsys.NetworkRemoveRoute(PEER_IP_SHARE_LOCAL_NET_ID, peer.iface.c_str(),
                                                peer.addresses.subnet.c_str(), PEER_DIRECT_NEXT_HOP);
        if (released("IPv4 route", ret, missing(ret)))
            peer.routeAdded = false;
        else
            ok = false;
    }
    if (!peer.routeAdded && peer.interfaceAdded) {
        int32_t ret = netsys.NetworkRemoveInterface(PEER_IP_SHARE_LOCAL_NET_ID, peer.iface.c_str());
        if (released("local interface", ret, missing(ret)))
            peer.interfaceAdded = false;
        else
            ok = false;
    }
    if (peer.addressAdded) {
        int32_t ret = sameInterface ?
            netsys.DelInterfaceAddress(peer.iface.c_str(), peer.addresses.gateway, PEER_PREFIX_LENGTH) : -ENODEV;
        if (released("IPv4 address", ret, missing(ret)))
            peer.addressAdded = false;
        else
            ok = false;
    }
    return ok;
}

int32_t NearlinkIpShareController::ConfigureGatewayPeerIpv4(PeerAddressContext &peer, const NetLinkInfo *upstream)
{
    if (peer.dhcpStarted && peer.ipv4Error != 0) {
        int32_t ret = StopDhcpServer(peer.iface.c_str());
        if (ret != DHCP_SUCCESS)
            return ret;
        peer.dhcpStarted = false;
    }
    if (upstream)
        for (const auto &address : upstream->netAddrList_) {
            if (address.family_ == AF_INET && peer.addresses.Conflicts(address.address_, address.prefixlen_)) {
                // Revoke only this family's DHCP/address resources on a changed upstream conflict.
                if (peer.dhcpStarted && StopDhcpServer(peer.iface.c_str()) == DHCP_SUCCESS)
                    peer.dhcpStarted = false;
                if (peer.addressAdded && NetsysController::GetInstance().DelInterfaceAddress(
                                             peer.iface.c_str(), peer.addresses.gateway, PEER_PREFIX_LENGTH) == 0)
                    peer.addressAdded = false;
                return NETMANAGER_EXT_ERR_PARAMETER_ERROR;
            }
        }
    auto &netsys = NetsysController::GetInstance();
    int32_t ret = 0;
    if (!peer.interfaceAdded) {
        ret = netsys.NetworkAddInterface(PEER_IP_SHARE_LOCAL_NET_ID, peer.iface.c_str());
        if (ret != 0)
            return ret;
        peer.interfaceAdded = true;
    }
    if (!peer.addressAdded) {
        ret = netsys.AddInterfaceAddress(peer.iface.c_str(), peer.addresses.gateway, PEER_PREFIX_LENGTH);
        if (ret != 0)
            return ret;
        peer.addressAdded = true;
    }
    if (!peer.routeAdded) {
        ret = netsys.NetworkAddRoute(PEER_IP_SHARE_LOCAL_NET_ID, peer.iface.c_str(), peer.addresses.subnet.c_str(),
                                     PEER_DIRECT_NEXT_HOP);
        if (ret != 0)
            return ret;
        peer.routeAdded = true;
    }
    if (!peer.dhcpStarted) {
        DhcpRange range{};
        range.iptype = PEER_DHCP_IPV4;
        range.leaseHours = 6;
        if (strcpy_s(range.strTagName, sizeof(range.strTagName), peer.iface.c_str()) != EOK ||
            strcpy_s(range.strStartip, sizeof(range.strStartip), peer.addresses.start.c_str()) != EOK ||
            strcpy_s(range.strEndip, sizeof(range.strEndip), peer.addresses.end.c_str()) != EOK ||
            strcpy_s(range.strSubnet, sizeof(range.strSubnet), PEER_SUBNET_MASK) != EOK)
            return NETMANAGER_EXT_ERR_PARAMETER_ERROR;
        ret = SetDhcpRange(peer.iface.c_str(), &range);
        if (ret != DHCP_SUCCESS)
            return ret;
        // A failed start can still own a worker/range. Release only after a successful stop.
        peer.dhcpStarted = true;
        ret = StartDhcpServer(peer.iface.c_str());
        if (ret != DHCP_SUCCESS) {
            if (StopDhcpServer(peer.iface.c_str()) == DHCP_SUCCESS)
                peer.dhcpStarted = false;
            return ret;
        }
    }
    return 0;
}

void NearlinkIpShareController::ConfigureGatewayPeers(const OHOS::Nearlink::NearlinkIpShareStatus &link)
{
    if (link.generation != linkGeneration_ || link.sequence < linkSequence_ ||
        link.peerLinks.size() > static_cast<size_t>(maxTerminals_))
        return;
    std::set<uint32_t> live;
    for (const auto &entry : link.peerLinks) {
        if (entry.slot >= static_cast<uint32_t>(maxTerminals_) || !entry.generation ||
            entry.ifaceName != "sleip" + std::to_string(entry.slot) ||
            (entry.selectedMode != 1 && entry.selectedMode != 3) ||
            (entry.selectedMode & status_.requestedMode) != entry.selectedMode || !live.insert(entry.slot).second)
            return;
    }
    {
        std::lock_guard lock(mutex_);
        gatewayPeerLinks_.clear();
        for (const auto &entry : link.peerLinks) {
            NearlinkIpSharePeerStatus peer;
            peer.slot = entry.slot;
            peer.generation = entry.generation;
            peer.ifaceName = entry.ifaceName;
            peer.selectedMode = entry.selectedMode;
            peer.state = entry.releasing ? 4 : (entry.active ? 1 : 0);
            gatewayPeerLinks_.push_back(peer);
        }
    }
    for (auto it = addressPeers_.begin(); it != addressPeers_.end();) {
        auto entry = std::find_if(link.peerLinks.begin(), link.peerLinks.end(), [&](const auto &e) {
            return !e.releasing && e.slot == it->first && e.generation == it->second.generation;
        });
        if (entry == link.peerLinks.end()) {
            it->second.releasing = true;
            if (CleanupGatewayPeer(it->second)) {
                it = addressPeers_.erase(it);
                continue;
            }
        }
        ++it;
    }
    for (const auto &entry : link.peerLinks) {
        if (entry.releasing && addressPeers_.count(entry.slot) == 0)
            OHOS::Nearlink::NearlinkIpShareClient::GetInstance().CompleteGatewayPeerRelease(entry.generation);
    }
    NetHandle network;
    NetLinkInfo upstream;
    bool hasUpstream = NetConnClient::GetInstance().GetDefaultNet(network) == 0 &&
                       NetConnClient::GetInstance().GetConnectionProperties(network, upstream) == 0;
    if (!dnsProxyStarted_ && !link.peerLinks.empty())
        dnsProxyStarted_ = NetsysController::GetInstance().StartDnsProxyListen() == 0;
    bool first = true;
    for (const auto &entry : link.peerLinks) {
        if (entry.releasing || !entry.active)
            continue;
        unsigned index = if_nametoindex(entry.ifaceName.c_str());
        if (!index)
            continue; // Release notification can follow TUN destruction.
        auto found = addressPeers_.find(entry.slot);
        if (found != addressPeers_.end() &&
            (found->second.generation != entry.generation || found->second.ifindex != index)) {
            if (!CleanupGatewayPeer(found->second))
                continue;
            addressPeers_.erase(found);
        }
        auto &peer = addressPeers_[entry.slot];
        peer.releasing = false;
        if (!peer.generation) {
            peer.generation = entry.generation;
            peer.slot = entry.slot;
            peer.ifindex = index;
            peer.mode = entry.selectedMode;
            peer.iface = entry.ifaceName;
            if (!NearlinkPeerAddresses::Allocate(configuration_.GetNearlinkIpv4Pool(),
                                                 configuration_.GetNearlinkIpv6Pool(), maxTerminals_, entry.slot,
                                                 peer.addresses, peer.mode == 3)) {
                addressPeers_.erase(entry.slot);
                continue;
            }
        }
        int32_t v4 = ConfigureGatewayPeerIpv4(peer, hasUpstream ? &upstream : nullptr);
        peer.ipv4Error = v4;
        if (peer.mode == 3) {
            if (!peer.ipv6Prepared) {
                std::string local;
                peer.ipv6Prepared = OHOS::Nearlink::NearlinkHost::GetInstance().GetLocalAddress(local) == 0 &&
                                    peer.ipv6.Prepare(true, local, peer.iface, peer.addresses.prefix);
            }
            // RA is reconciled once below, after this peer's upstream ledger is committed.
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
        NETMGR_EXT_LOG_I("[NearlinkIpShare][PeerAddress] slot=%{public}u generation=%{public}llu "
                         "iface=%{public}s ifindex=%{public}u mode=%{public}d dhcp=%{public}d ipv4Code=%{public}d "
                         "ra=%{public}d ipv6Code=%{public}d",
                         peer.slot, static_cast<unsigned long long>(peer.generation), peer.iface.c_str(), peer.ifindex,
                         peer.mode, peer.dhcpStarted, v4, peer.ipv6Ready, peer.ipv6Error);
        if (first) {
            std::lock_guard lock(mutex_);
            status_.ipv4Address = v4 == 0 ? peer.addresses.gateway : "";
            status_.ipv4.configurationAvailable = v4 == 0;
            status_.ipv4.phase = v4 == 0 ? 2 : 3;
            status_.ipv4.hasError = v4 != 0;
            status_.ipv4.error.stage = v4 ? "DHCP" : "";
            status_.ipv4.error.code = v4;
            status_.ipv6.configurationAvailable = peer.ipv6Ready;
            status_.ipv6.phase = peer.mode == 3 ? (peer.ipv6Ready ? 2 : (peer.ipv6Error ? 3 : 1)) : 0;
            status_.ipv6.hasError = peer.ipv6Error != 0;
            status_.ipv6.error.stage = peer.ipv6Error ? "PREFIX" : "";
            status_.ipv6.error.code = peer.ipv6Error;
            status_.ipv6.error.retryable = peer.ipv6Error != 0;
            status_.ifaceName = peer.iface;
            first = false;
        }
    }
    {
        std::lock_guard lock(mutex_);
        status_.serviceReady = link.serviceReady;
        status_.hasUpstream = false;
        status_.ipv4.externalAvailable = status_.ipv6.externalAvailable = false;
        if (first) {
            status_.ifaceName.clear();
            status_.ipv4Address.clear();
            status_.ipv4 = {};
            status_.ipv6 = {};
        }
    }
    ConfigureGatewayPeerUpstreams(hasUpstream ? &upstream : nullptr, hasUpstream ? network.GetNetId() : -1);
    bool ready;
    {
        std::lock_guard lock(mutex_);
        ready = status_.hasUpstream;
    }
    Publish(link.serviceReady ? (ready ? NearlinkIpShareState::SERVING : NearlinkIpShareState::SERVING_NO_UPSTREAM)
                              : NearlinkIpShareState::STARTING);
}

void NearlinkIpShareController::RefreshGatewayStatusLocked()
{
    // Called by Publish on the serialized tracker worker while holding the snapshot lock.
    // Include NearLink's reserved seats and retained L3 cleanup ownership, ordered by slot.
    if (!multiGateway_) {
        status_.peers.clear();
        status_.occupiedTerminals = status_.activeTerminals = 0;
        if (status_.selectedMode == 0) return;
        NearlinkIpSharePeerStatus peer;
        peer.generation = status_.generation;
        peer.sequence = status_.sequence;
        peer.peerId = "slot:0/" + std::to_string(peer.generation);
        peer.contextId = status_.contextId + "/" + peer.peerId;
        peer.ifaceName = "sleip0";
        peer.selectedMode = status_.selectedMode;
        peer.hasUpstream = status_.hasUpstream;
        peer.ipv4 = status_.ipv4;
        peer.ipv6 = status_.ipv6;
        bool configured = peer.ipv4.configurationAvailable &&
            (peer.selectedMode != 3 || peer.ipv6.configurationAvailable);
        peer.state = status_.state == NearlinkIpShareState::STOPPING ? 4 :
            (configured ? (peer.hasUpstream ? 2 : 3) : 1);
        if (peer.state == 4) {
            peer.hasUpstream = false;
            peer.ipv4 = {};
            peer.ipv6 = {};
        }
        status_.peers.push_back(peer);
        status_.occupiedTerminals = 1;
        status_.activeTerminals = peer.state == 2;
        return;
    }
    std::map<uint32_t, NearlinkIpSharePeerStatus> peers;
    for (const auto &link : gatewayPeerLinks_) peers.emplace(link.slot, link);
    for (const auto &[slot, local] : addressPeers_) {
        auto &peer = peers[slot];
        peer.slot = slot;
        peer.generation = local.generation;
        peer.ifaceName = local.iface;
        peer.selectedMode = local.mode;
        peer.hasUpstream = !local.releasing && local.interfaceForwarding && local.natEnabled &&
            forwardingEnabled_ && dnsUpstreamReady_;
        if (local.releasing || status_.state == NearlinkIpShareState::STOPPING) {
            peer.state = 4;
            peer.hasUpstream = false;
            peer.ipv4 = {};
            peer.ipv6 = {};
            continue;
        }
        peer.ipv4.configurationAvailable = local.dhcpStarted && local.addressAdded && local.ipv4Error == 0;
        peer.ipv4.phase = local.ipv4Error ? 3 : (peer.ipv4.configurationAvailable ? 2 : 1);
        if (local.addressAdded) {
            NearlinkIpShareAddress address;
            address.address = local.addresses.gateway;
            address.prefixLength = 24;
            peer.ipv4.addresses.push_back(address);
        }
        peer.ipv4.hasError = local.ipv4Error != 0 || local.upstreamError != 0;
        peer.ipv4.error = {4, 1, local.ipv4Error ? "DHCP" : "UPSTREAM",
                          local.ipv4Error ? local.ipv4Error : local.upstreamError, true};
        if (local.mode == 3) {
            peer.ipv6.configurationAvailable = local.ipv6Ready;
            peer.ipv6.phase = local.ipv6Ready ? 2 : (local.ipv6Error ? 3 : 1);
            auto addressText = local.ipv6.Gateway();
            if (!addressText.empty()) {
                NearlinkIpShareAddress address;
                address.address = addressText;
                address.prefixLength = 64;
                address.scopeId = local.ifindex;
                peer.ipv6.addresses.push_back(address);
            }
            peer.ipv6.hasError = local.ipv6Error != 0 || local.upstreamError != 0;
            peer.ipv6.error = {4, 2, local.ipv6Error ? "PREFIX" : "UPSTREAM",
                              local.ipv6Error ? local.ipv6Error : local.upstreamError, true};
        }
        bool configured = peer.ipv4.configurationAvailable &&
            (local.mode != 3 || peer.ipv6.configurationAvailable);
        bool routed = peer.hasUpstream && (local.mode != 3 || local.ipv6Routed);
        bool anyConfigured = peer.ipv4.configurationAvailable || peer.ipv6.configurationAvailable;
        peer.state = configured ? (routed ? 2 : 3) :
            (peer.ipv4.hasError || peer.ipv6.hasError ? (anyConfigured ? 3 : 5) : 1);
        // G has forwarding evidence, not a per-terminal reachability probe. Leave validation UNKNOWN.
    }
    status_.peers.clear();
    status_.activeTerminals = 0;
    for (auto &[slot, peer] : peers) {
        if (status_.state == NearlinkIpShareState::STOPPING) {
            peer.state = 4;
            peer.hasUpstream = false;
            peer.ipv4 = {};
            peer.ipv6 = {};
        }
        peer.peerId = "slot:" + std::to_string(slot) + "/" + std::to_string(peer.generation);
        peer.contextId = status_.contextId + "/" + peer.peerId;
        peer.sequence = status_.sequence;
        status_.activeTerminals += peer.state == 2;
        status_.peers.push_back(peer);
    }
    status_.occupiedTerminals = status_.peers.size();
}

} // namespace OHOS::NetManagerStandard
