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
    bool ok = true;
    if (peer.dhcpStarted) {
        if (StopDhcpServer(peer.iface.c_str()) == DHCP_SUCCESS)
            peer.dhcpStarted = false;
        else
            ok = false;
    }
    if (peer.ipv6.Cleanup())
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
        if (missing(ret))
            peer.routeAdded = false;
        else
            ok = false;
    }
    if (!peer.routeAdded && peer.interfaceAdded) {
        if (missing(netsys.NetworkRemoveInterface(PEER_IP_SHARE_LOCAL_NET_ID, peer.iface.c_str())))
            peer.interfaceAdded = false;
        else
            ok = false;
    }
    if (peer.addressAdded) {
        if (!sameInterface ||
            missing(netsys.DelInterfaceAddress(peer.iface.c_str(), peer.addresses.gateway, PEER_PREFIX_LENGTH)))
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
    if (link.generation != linkGeneration_ || link.peerLinks.size() > static_cast<size_t>(maxTerminals_))
        return;
    std::set<uint32_t> live;
    for (const auto &entry : link.peerLinks) {
        if (entry.slot >= static_cast<uint32_t>(maxTerminals_) || !entry.generation ||
            entry.ifaceName != "sleip" + std::to_string(entry.slot) ||
            (entry.selectedMode != 1 && entry.selectedMode != 3) ||
            (entry.selectedMode & status_.requestedMode) != entry.selectedMode || !live.insert(entry.slot).second)
            return;
    }
    for (auto it = addressPeers_.begin(); it != addressPeers_.end();) {
        auto entry = std::find_if(link.peerLinks.begin(), link.peerLinks.end(), [&](const auto &e) {
            return !e.releasing && e.slot == it->first && e.generation == it->second.generation;
        });
        if (entry == link.peerLinks.end()) {
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
        if (entry.releasing)
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
            // S2 announces only a local logical link; routed upstream ownership is an S3 gate.
            peer.ipv6Ready = peer.ipv6Prepared && peer.ipv6.Advertise(nullptr, false, dnsProxyStarted_);
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
                         "ra=%{public}d ipv6Code=%{public}d ipv6LocalOnly=1",
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
    Publish(link.serviceReady ? NearlinkIpShareState::SERVING_NO_UPSTREAM : NearlinkIpShareState::STARTING);
}

} // namespace OHOS::NetManagerStandard
