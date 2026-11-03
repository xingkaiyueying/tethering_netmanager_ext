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

#include <algorithm>
#include <cerrno>
#include <net/if.h>
#include <ifaddrs.h>
#include <set>

// Include libc interface definitions before Netsys pulls in <linux/if.h>.
#include "dhcp_c_api.h"
#include "nearlink_host.h"
#include "nearlink_ipshare_controller.h"
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

bool SelectSharedSubnet(const NetworkShareConfiguration &configuration, unsigned capacity,
                        const NetLinkInfo *upstream, NearlinkPeerAddresses &selected, bool dual,
                        const std::vector<NearlinkPeerAddresses> &retired = {})
{
    uint8_t pool[4]{};
    unsigned bits = 0;
    if (!NearlinkPeerAddresses::ParsePool(configuration.GetNearlinkIpv4Pool(), AF_INET, pool, bits) ||
        bits < 8 || bits > 24) {
        return false;
    }
    std::vector<std::pair<std::string, unsigned>> occupied;
    ifaddrs *interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {
        return false;
    }
    for (auto *entry = interfaces; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET || !entry->ifa_netmask ||
            strcmp(entry->ifa_name, "sleip0") == 0) {
            continue;
        }
        const auto &address = reinterpret_cast<const sockaddr_in *>(entry->ifa_addr)->sin_addr;
        uint32_t mask = ntohl(reinterpret_cast<const sockaddr_in *>(entry->ifa_netmask)->sin_addr.s_addr);
        unsigned prefix = 0;
        while (mask & 0x80000000u) {
            ++prefix;
            mask <<= 1;
        }
        char text[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &address, text, sizeof(text))) {
            occupied.emplace_back(text, prefix);
        }
    }
    freeifaddrs(interfaces);
    if (upstream) {
        for (const auto &address : upstream->netAddrList_) {
            if (address.family_ == AF_INET) {
                occupied.emplace_back(address.address_, address.prefixlen_);
            }
        }
    }
    uint32_t base = (uint32_t(pool[0]) << 24) | (uint32_t(pool[1]) << 16) | (uint32_t(pool[2]) << 8);
    for (unsigned subnet = 0; subnet < (1u << (24 - bits)); ++subnet) {
        uint32_t address = htonl(base + subnet * 256);
        char text[INET_ADDRSTRLEN]{};
        if (!inet_ntop(AF_INET, &address, text, sizeof(text)) ||
            !NearlinkPeerAddresses::Allocate(std::string(text) + "/24", configuration.GetNearlinkIpv6Pool(),
                                             capacity, 0, selected, dual)) {
            return false;
        }
        bool available = std::none_of(occupied.begin(), occupied.end(), [&](const auto &other) {
            return selected.Conflicts(other.first, other.second);
        }) && std::none_of(retired.begin(), retired.end(), [&](const auto &old) {
            return selected.subnet == old.subnet;
        });
        if (available) {
            return true;
        }
    }
    return false;
}

} // namespace
bool NearlinkIpShareController::CleanupGatewayPeer(SharedNetworkContext &peer)
{
    if (peer.upstreamSignal) {
        peer.upstreamSignal->Reset();
    }
    if (CleanupGatewayPeerUpstream(peer) != 0) {
        return false;
    }
    bool ok = true;
    auto released = [&peer](const char *resource, int32_t ret, bool success) {
        NETMGR_EXT_LOG_I(
            "[NearlinkIpShare][PeerCleanup] slot=%{public}u generation=%{public}llu "
            "iface=%{public}s ifindex=%{public}u resource=%{public}s code=%{public}d released=%{public}d",
            peer.slot, static_cast<unsigned long long>(peer.generation), peer.iface.c_str(), peer.ifindex, resource,
            ret, success);
        return success;
    };
    if (peer.dhcpStarted) {
        int32_t ret = StopDhcpServer(peer.iface.c_str());
        if (released("DHCP", ret, ret == DHCP_SUCCESS)) {
            peer.dhcpStarted = false;
        } else {
            ok = false;
        }
    }
    bool ipv6Released = peer.ipv6.Cleanup();
    if (released("IPv6", ipv6Released ? 0 : NETMANAGER_EXT_ERR_OPERATION_FAILED, ipv6Released)) {
        peer.ipv6Prepared = peer.ipv6Ready = false;
    } else {
        ok = false;
    }
    auto &netsys = NetsysController::GetInstance();
    auto missing = [](int32_t result) {
        return result == 0 || result == -ENODEV || result == -ESRCH || result == -EADDRNOTAVAIL;
    };
    // An interface name may have been reused. Old addresses are never deleted from the new ifindex.
    bool sameInterface = if_nametoindex(peer.iface.c_str()) == peer.ifindex;
    if (peer.routeAdded) {
        int32_t ret = netsys.NetworkRemoveRoute(PEER_IP_SHARE_LOCAL_NET_ID, peer.iface.c_str(),
                                                peer.addresses.subnet.c_str(), PEER_DIRECT_NEXT_HOP);
        if (released("IPv4 route", ret, missing(ret))) {
            peer.routeAdded = false;
        } else {
            ok = false;
        }
    }
    for (auto it = peer.retiredIpv4.begin(); it != peer.retiredIpv4.end();) {
        int32_t route = netsys.NetworkRemoveRoute(PEER_IP_SHARE_LOCAL_NET_ID, peer.iface.c_str(),
                                                 it->subnet.c_str(), PEER_DIRECT_NEXT_HOP);
        int32_t address = sameInterface ? netsys.DelInterfaceAddress(peer.iface.c_str(), it->gateway,
                                                                    PEER_PREFIX_LENGTH) : -ENODEV;
        if (missing(route) && missing(address)) {
            it = peer.retiredIpv4.erase(it);
        } else {
            ok = false;
            ++it;
        }
    }
    if (!peer.routeAdded && peer.retiredIpv4.empty() && peer.interfaceAdded) {
        int32_t ret = netsys.NetworkRemoveInterface(PEER_IP_SHARE_LOCAL_NET_ID, peer.iface.c_str());
        if (released("local interface", ret, missing(ret))) {
            peer.interfaceAdded = false;
        } else {
            ok = false;
        }
    }
    if (peer.addressAdded) {
        int32_t ret = sameInterface
                          ? netsys.DelInterfaceAddress(peer.iface.c_str(), peer.addresses.gateway, PEER_PREFIX_LENGTH)
                          : -ENODEV;
        if (released("IPv4 address", ret, missing(ret))) {
            peer.addressAdded = false;
        } else {
            ok = false;
        }
    }
    return ok;
}

int32_t NearlinkIpShareController::ConfigureGatewayPeerIpv4(SharedNetworkContext &peer, const NetLinkInfo *upstream)
{
    if (peer.dhcpStarted && peer.ipv4Error != 0) {
        int32_t ret = StopDhcpServer(peer.iface.c_str());
        if (ret != DHCP_SUCCESS) {
            return ret;
        }
        peer.dhcpStarted = false;
    }
    if (upstream) {
        for (const auto &address : upstream->netAddrList_) {
            if (address.family_ == AF_INET && peer.addresses.Conflicts(address.address_, address.prefixlen_)) {
                NearlinkPeerAddresses replacement;
                if (peer.retiredIpv4.size() >= 4 ||
                    !SelectSharedSubnet(configuration_, maxTerminals_, upstream, replacement, peer.mode == 3,
                                        peer.retiredIpv4)) {
                    return NETMANAGER_EXT_ERR_PARAMETER_ERROR;
                }
                if (peer.dhcpStarted && StopDhcpServer(peer.iface.c_str()) != DHCP_SUCCESS) {
                    return NETMANAGER_EXT_ERR_OPERATION_FAILED;
                }
                peer.dhcpStarted = false;
                // Install the new primary before retaining the previous subnet for bounded
                // control delivery. The old route/address drain when their members renew.
                auto &netsys = NetsysController::GetInstance();
                if (peer.addressAdded && netsys.DelInterfaceAddress(peer.iface.c_str(), peer.addresses.gateway,
                                                                    PEER_PREFIX_LENGTH) != 0) {
                    return NETMANAGER_EXT_ERR_OPERATION_FAILED;
                }
                peer.retiredIpv4.push_back(peer.addresses);
                peer.addresses = replacement;
                peer.addressAdded = peer.routeAdded = false;
                NETMGR_EXT_LOG_I("[NearlinkIpShare][SharedNetwork] IPv4 subnet changed generation=%{public}llu "
                                 "retired=%{public}zu", static_cast<unsigned long long>(peer.generation),
                                 peer.retiredIpv4.size());
                break;
            }
        }
    }
    auto &netsys = NetsysController::GetInstance();
    int32_t ret = 0;
    if (!peer.interfaceAdded) {
        ret = netsys.NetworkAddInterface(PEER_IP_SHARE_LOCAL_NET_ID, peer.iface.c_str());
        if (ret != 0) {
            return ret;
        }
        peer.interfaceAdded = true;
    }
    if (!peer.addressAdded) {
        ret = netsys.AddInterfaceAddress(peer.iface.c_str(), peer.addresses.gateway, PEER_PREFIX_LENGTH);
        if (ret != 0) {
            return ret;
        }
        peer.addressAdded = true;
    }
    for (const auto &old : peer.retiredIpv4) {
        int32_t retained = netsys.AddInterfaceAddress(peer.iface.c_str(), old.gateway, PEER_PREFIX_LENGTH);
        if (retained != 0 && retained != -EEXIST) {
            return retained;
        }
    }
    if (!peer.routeAdded) {
        ret = netsys.NetworkAddRoute(PEER_IP_SHARE_LOCAL_NET_ID, peer.iface.c_str(), peer.addresses.subnet.c_str(),
                                     PEER_DIRECT_NEXT_HOP);
        if (ret != 0) {
            return ret;
        }
        peer.routeAdded = true;
    }
    if (!peer.dhcpStarted) {
        DhcpRange range{};
        range.iptype = PEER_DHCP_IPV4;
        range.leaseHours = 6;
        if (strcpy_s(range.strTagName, sizeof(range.strTagName), peer.iface.c_str()) != EOK ||
            strcpy_s(range.strStartip, sizeof(range.strStartip), peer.addresses.start.c_str()) != EOK ||
            strcpy_s(range.strEndip, sizeof(range.strEndip), peer.addresses.end.c_str()) != EOK ||
            strcpy_s(range.strSubnet, sizeof(range.strSubnet), PEER_SUBNET_MASK) != EOK) {
            return NETMANAGER_EXT_ERR_PARAMETER_ERROR;
        }
        ret = SetDhcpRange(peer.iface.c_str(), &range);
        if (ret != DHCP_SUCCESS) {
            return ret;
        }
        // A failed start can still own a worker/range. Release only after a successful stop.
        peer.dhcpStarted = true;
        ret = StartDhcpServer(peer.iface.c_str());
        if (ret != DHCP_SUCCESS) {
            if (StopDhcpServer(peer.iface.c_str()) == DHCP_SUCCESS) {
                peer.dhcpStarted = false;
            }
            return ret;
        }
    }
    return 0;
}

void NearlinkIpShareController::ConfigureGatewayPeerAddress(SharedNetworkContext &peer, const NetLinkInfo *upstream)
{
    const auto previousIpv4 = peer.ipv4Error;
    const auto previousIpv6 = peer.ipv6Error;
    const bool hadDhcp = peer.dhcpStarted;
    int32_t v4 = ConfigureGatewayPeerIpv4(peer, upstream);
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
            if (peer.ipv6PendingSince == std::chrono::steady_clock::time_point{}) {
                peer.ipv6PendingSince = now;
            }
            peer.ipv6Error =
                now - peer.ipv6PendingSince >= std::chrono::seconds(10) ? NETMANAGER_EXT_ERR_OPERATION_FAILED : 0;
        }
    }
    if (hadDhcp != peer.dhcpStarted || previousIpv4 != peer.ipv4Error || previousIpv6 != peer.ipv6Error) {
        NETMGR_EXT_LOG_I(
            "[NearlinkIpShare][PeerAddress] slot=%{public}u iface=%{public}s "
            "dhcp=%{public}d ipv4Code=%{public}d ipv6Code=%{public}d",
            peer.slot, peer.iface.c_str(), peer.dhcpStarted, peer.ipv4Error, peer.ipv6Error);
    }
}

void NearlinkIpShareController::ProjectGatewayPeerLocked(const SharedNetworkContext &peer)
{
    status_.ipv4Address = peer.ipv4Error == 0 ? peer.addresses.gateway : "";
    status_.ipv4.configurationAvailable = peer.ipv4Error == 0;
    status_.ipv4.phase = peer.ipv4Error == 0 ? 2 : 3;
    status_.ipv4.hasError = peer.ipv4Error != 0;
    status_.ipv4.error.stage = peer.ipv4Error ? "DHCP" : "";
    status_.ipv4.error.code = peer.ipv4Error;
    status_.ipv6.configurationAvailable = peer.ipv6Ready;
    status_.ipv6.phase = peer.mode == 3 ? (peer.ipv6Ready ? 2 : (peer.ipv6Error ? 3 : 1)) : 0;
    status_.ipv6.hasError = peer.ipv6Error != 0;
    status_.ipv6.error.stage = peer.ipv6Error ? "PREFIX" : "";
    status_.ipv6.error.code = peer.ipv6Error;
    status_.ipv6.error.retryable = peer.ipv6Error != 0;
    status_.ifaceName = peer.iface;
}

void NearlinkIpShareController::ConfigureGatewayPeers(const OHOS::Nearlink::NearlinkIpShareStatus &link)
{
    {
        std::lock_guard lock(mutex_);
        if (stopRequested_ || shuttingDown_ || status_.role != NearlinkIpShareRole::GATEWAY) {
            return;
        }
    }
    if (link.generation != linkGeneration_ || link.sequence < linkSequence_ ||
        link.peerLinks.size() > static_cast<size_t>(maxTerminals_)) {
        return;
    }
    std::set<uint32_t> live;
    for (const auto &entry : link.peerLinks) {
        if (entry.slot >= static_cast<uint32_t>(maxTerminals_) || !entry.generation || entry.ifaceName != "sleip0" ||
            (entry.selectedMode != 1 && entry.selectedMode != 3) ||
            (entry.selectedMode & status_.requestedMode) != entry.selectedMode || !live.insert(entry.slot).second) {
            return;
        }
    }
    gatewayPeerLinks_.clear();
    for (auto it = addressPeers_.begin(); it != addressPeers_.end();) {
        auto entry = std::find_if(link.peerLinks.begin(), link.peerLinks.end(), [&](const auto &value) {
            return !value.releasing && value.slot == it->first && value.generation == it->second.generation;
        });
        if (entry == link.peerLinks.end()) {
            NETMGR_EXT_LOG_I("[NearlinkIpShare][SharedNetwork] member removed slot=%{public}u epoch=%{public}llu",
                             it->first, static_cast<unsigned long long>(it->second.generation));
            it = addressPeers_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto &entry : link.peerLinks) {
        NearlinkIpSharePeerStatus projected;
        projected.slot = entry.slot;
        projected.generation = entry.generation;
        projected.ifaceName = entry.ifaceName;
        projected.selectedMode = entry.selectedMode;
        projected.state = entry.releasing ? 4 : (entry.active ? 1 : 0);
        gatewayPeerLinks_.push_back(projected);
        if (entry.releasing) {
            OHOS::Nearlink::NearlinkIpShareClient::GetInstance().CompleteGatewayPeerRelease(entry.generation);
            continue;
        }
        auto &member = addressPeers_[entry.slot];
        member.generation = entry.generation;
        member.slot = entry.slot;
        member.iface = "sleip0";
        member.mode = entry.selectedMode;
        member.active = entry.active;
        member.ipv4Address = entry.ipv4Address;
        member.ipv4LeaseLifetime = entry.ipv4LeaseLifetime;
        member.ipv6Addresses.clear();
        for (const auto &evidence : entry.ipv6Addresses) {
            NearlinkIpShareAddress address;
            address.address = evidence.address;
            address.prefixLength = 64;
            address.origin = 2;
            address.dadState = 2;
            address.preferredLifetime = evidence.preferredLifetime;
            address.validLifetime = evidence.validLifetime;
            member.ipv6Addresses.push_back(address);
        }
    }
    auto &network = gatewayNetwork_;
    const unsigned index = if_nametoindex("sleip0");
    if (!index) {
        if (link.serviceReady) {
            Fail("SHARED_IFACE", NETMANAGER_EXT_ERR_OPERATION_FAILED);
        }
        return;
    }
    if (network.generation && (network.generation != link.generation || network.ifindex != index)) {
        // A replacement interface affects every member. Retain cleanup ownership on failure.
        Fail("SHARED_IFINDEX", NETMANAGER_EXT_ERR_OPERATION_FAILED);
        return;
    }
    if (!network.generation) {
        network.generation = link.generation;
        network.ifindex = index;
        network.iface = "sleip0";
        if (!SelectSharedSubnet(configuration_, maxTerminals_, nullptr, network.addresses,
                                status_.requestedMode == 3)) {
            Fail("SHARED_POOL", NETMANAGER_EXT_ERR_PARAMETER_ERROR);
            return;
        }
        NETMGR_EXT_LOG_I("[NearlinkIpShare][SharedNetwork] created generation=%{public}llu ifindex=%{public}u "
                         "capacity=%{public}d leases=253", static_cast<unsigned long long>(network.generation),
                         index, maxTerminals_);
    }
    for (auto &[slot, member] : addressPeers_) {
        member.ifindex = index;
        for (auto &address : member.ipv6Addresses) {
            address.scopeId = index;
        }
    }
    bool dual = std::any_of(addressPeers_.begin(), addressPeers_.end(), [](const auto &item) {
        return item.second.mode == 3 && !item.second.releasing;
    });
    if (!dual && network.ipv6Prepared && network.ipv6.Cleanup()) {
        network.ipv6Prepared = network.ipv6Ready = network.ipv6Routed = false;
    }
    network.mode = dual || network.ipv6Prepared ? 3 : 1;
    NetHandle handle;
    NetLinkInfo upstream;
    bool present = NetConnClient::GetInstance().GetDefaultNet(handle) == 0 &&
                   NetConnClient::GetInstance().GetConnectionProperties(handle, upstream) == 0;
    ConfigureGatewayPeerAddress(network, present ? &upstream : nullptr);
    // Keep retiring subnets only while a member still holds a matching lease.
    auto &netsys = NetsysController::GetInstance();
    for (auto it = network.retiredIpv4.begin(); it != network.retiredIpv4.end();) {
        bool occupied = std::any_of(addressPeers_.begin(), addressPeers_.end(), [&](const auto &member) {
            return !member.second.ipv4Address.empty() && it->Conflicts(member.second.ipv4Address, 32);
        });
        if (occupied) {
            ++it;
            continue;
        }
        int32_t route = netsys.NetworkRemoveRoute(PEER_IP_SHARE_LOCAL_NET_ID, "sleip0", it->subnet.c_str(),
                                                 PEER_DIRECT_NEXT_HOP);
        int32_t address = netsys.DelInterfaceAddress("sleip0", it->gateway, PEER_PREFIX_LENGTH);
        auto removed = [](int32_t code) {
            return code == 0 || code == -ESRCH || code == -ENODEV || code == -EADDRNOTAVAIL;
        };
        if (removed(route) && removed(address)) {
            it = network.retiredIpv4.erase(it);
        } else {
            ++it;
        }
    }

    {
        std::lock_guard lock(mutex_);
        ProjectGatewayPeerLocked(network);
        status_.serviceReady = link.serviceReady;
        status_.hasUpstream = false;
        status_.ipv4.externalAvailable = status_.ipv6.externalAvailable = false;
    }
    ConfigureGatewayPeerUpstreams(present ? &upstream : nullptr, present ? handle.GetNetId() : -1);
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
    if (status_.role != NearlinkIpShareRole::GATEWAY) {
        status_.peers.clear();
        status_.occupiedTerminals = status_.activeTerminals = 0;
        if (status_.selectedMode == 0) {
            return;
        }
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
        bool configured =
            peer.ipv4.configurationAvailable && (peer.selectedMode != 3 || peer.ipv6.configurationAvailable);
        peer.state =
            status_.state == NearlinkIpShareState::STOPPING ? 4 : (configured ? (peer.hasUpstream ? 2 : 3) : 1);
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
    for (const auto &link : gatewayPeerLinks_) {
        peers.emplace(link.slot, link);
    }
    for (const auto &[slot, local] : addressPeers_) {
        auto &peer = peers[slot];
        peer.slot = slot;
        peer.generation = local.generation;
        peer.ifaceName = local.iface;
        peer.selectedMode = local.mode;
        const auto &network = gatewayNetwork_;
        peer.hasUpstream = !local.releasing && network.interfaceForwarding && network.natEnabled &&
                           forwardingEnabled_ && dnsUpstreamReady_;
        if (local.releasing || status_.state == NearlinkIpShareState::STOPPING) {
            peer.state = 4;
            peer.hasUpstream = false;
            peer.ipv4 = {};
            peer.ipv6 = {};
            continue;
        }
        if (!local.active) {
            peer.state = 0;
            peer.hasUpstream = false;
            continue;
        }
        peer.ipv4.configurationAvailable = !local.ipv4Address.empty() && network.dhcpStarted && network.ipv4Error == 0;
        peer.ipv4.phase = network.ipv4Error ? 3 : (peer.ipv4.configurationAvailable ? 2 : 1);
        if (!local.ipv4Address.empty()) {
            NearlinkIpShareAddress address;
            address.address = local.ipv4Address;
            address.prefixLength = 24;
            address.origin = 1;
            address.preferredLifetime = address.validLifetime = local.ipv4LeaseLifetime;
            peer.ipv4.addresses.push_back(address);
        }
        peer.ipv4.hasError = network.ipv4Error != 0 || network.upstreamError != 0;
        peer.ipv4.error = {4, 1, network.ipv4Error ? "DHCP" : "UPSTREAM",
                           network.ipv4Error ? network.ipv4Error : network.upstreamError, true};
        if (local.mode == 3) {
            peer.ipv6.configurationAvailable = network.ipv6Ready && !local.ipv6Addresses.empty();
            peer.ipv6.phase = peer.ipv6.configurationAvailable ? 2 : (network.ipv6Error ? 3 : 1);
            peer.ipv6.addresses = local.ipv6Addresses;
            peer.ipv6.hasError = network.ipv6Error != 0 || network.upstreamError != 0;
            peer.ipv6.error = {4, 2, network.ipv6Error ? "PREFIX" : "UPSTREAM",
                               network.ipv6Error ? network.ipv6Error : network.upstreamError, true};
        }
        bool configured = peer.ipv4.configurationAvailable && (local.mode != 3 || peer.ipv6.configurationAvailable);
        bool routed = peer.hasUpstream && (local.mode != 3 || network.ipv6Routed);
        bool anyConfigured = peer.ipv4.configurationAvailable || peer.ipv6.configurationAvailable;
        peer.state =
            configured ? (routed ? 2 : 3) : (peer.ipv4.hasError || peer.ipv6.hasError ? (anyConfigured ? 3 : 5) : 1);
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
