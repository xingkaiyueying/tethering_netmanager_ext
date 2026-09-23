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
#include "nearlink_ipv6_runtime.h"
#include <net/if.h>
#include "netsys_controller.h"
#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <linux/if_link.h>
#include <linux/rtnetlink.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace OHOS::NetManagerStandard {
namespace {
constexpr const char *IFACE = "sleip0";
constexpr size_t MAX_PREFIXES = 4;
constexpr int PREFIX_VALID_SECONDS = 300;
constexpr int PREFIX_PREFERRED_SECONDS = 180;
constexpr int LOCAL_NETWORK_ID = 99;
constexpr unsigned UNUSABLE_ADDRESS_FLAGS = 0x04 | 0x08 | 0x40;
bool RouteRemoved(int32_t result)
{
    return result == 0 || result == -ESRCH;
}

bool AddressRemoved(int32_t result)
{
    return result == 0 || result == -EADDRNOTAVAIL || result == -ENODEV;
}

bool Prefix(const std::string &text, in6_addr &address)
{
    if (inet_pton(AF_INET6, text.c_str(), &address) != 1) {
        return false;
    }
    for (size_t i = 8; i < 16; ++i) {
        if (address.s6_addr[i]) {
            return false;
        }
    }
    return true;
}

bool Layer2Token(const std::string &layer2, in6_addr &token)
{
    if (layer2.size() != 17) {
        return false;
    }
    std::array<unsigned, 6> bytes{};
    int consumed = 0;
    if (std::sscanf(layer2.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x%n", &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4],
                    &bytes[5], &consumed) != 6 ||
        consumed != static_cast<int>(layer2.size())) {
        return false;
    }
    token = IN6ADDR_ANY_INIT;
    token.s6_addr[8] = static_cast<uint8_t>(bytes[0] ^ 0x02);
    token.s6_addr[9] = static_cast<uint8_t>(bytes[1]);
    token.s6_addr[10] = static_cast<uint8_t>(bytes[2]);
    token.s6_addr[11] = 0xff;
    token.s6_addr[12] = 0xfe;
    token.s6_addr[13] = static_cast<uint8_t>(bytes[3]);
    token.s6_addr[14] = static_cast<uint8_t>(bytes[4]);
    token.s6_addr[15] = static_cast<uint8_t>(bytes[5]);
    return true;
}

bool DeriveDownstreamPrefix(const NetLinkInfo *upstream, in6_addr &downstream, std::string &text)
{
    if (upstream == nullptr) {
        return false;
    }
    std::vector<in6_addr> global;
    std::vector<in6_addr> ula;
    auto addCandidate = [&global, &ula](in6_addr candidate) {
        bool isGlobal = (candidate.s6_addr[0] & 0xe0) == 0x20;
        bool isUla = (candidate.s6_addr[0] & 0xfe) == 0xfc;
        if (!isGlobal && !isUla) {
            return;
        }
        std::fill(candidate.s6_addr + 8, candidate.s6_addr + 16, 0);
        auto &list = isGlobal ? global : ula;
        if (std::none_of(list.begin(), list.end(), [&candidate](const auto &item) {
                return std::memcmp(item.s6_addr, candidate.s6_addr, 8) == 0;
            })) {
            list.push_back(candidate);
        }
    };
    for (const auto &address : upstream->netAddrList_) {
        in6_addr candidate{};
        if (address.family_ == AF_INET6 && address.prefixlen_ == 64 &&
            inet_pton(AF_INET6, address.address_.c_str(), &candidate) == 1) {
            addCandidate(candidate);
        }
    }
    if (global.empty() && ula.empty() && !upstream->ifaceName_.empty() && upstream->ifaceName_ != IFACE) {
        // Some upstream providers omit IPv6 addresses from NetLinkInfo. Consult only
        // the selected interface's current, usable /64 address; never scan other links.
        std::ifstream kernel("/proc/net/if_inet6");
        std::string hex, iface;
        unsigned index, length, scope, flags;
        while (kernel >> hex >> std::hex >> index >> length >> scope >> flags >> iface) {
            if (iface != upstream->ifaceName_ || length != 64 || scope != 0 ||
                (flags & UNUSABLE_ADDRESS_FLAGS) || hex.size() != 32 ||
                !std::all_of(hex.begin(), hex.end(), [](unsigned char c) { return std::isxdigit(c); })) {
                continue;
            }
            in6_addr candidate{};
            for (size_t i = 0; i < 16; ++i) {
                unsigned byte = 0;
                (void)std::sscanf(hex.c_str() + i * 2, "%2x", &byte);
                candidate.s6_addr[i] = static_cast<uint8_t>(byte);
            }
            addCandidate(candidate);
        }
    }
    auto &candidates = global.empty() ? ula : global;
    if (candidates.empty()) {
        return false;
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto &left, const auto &right) { return std::memcmp(left.s6_addr, right.s6_addr, 8) < 0; });
    downstream = candidates.front();
    if (downstream.s6_addr[7] == 0xff) {
        return false;
    }
    ++downstream.s6_addr[7]; // Match the existing PAN tethering prefix derivation.
    auto conflicts = [&downstream](const auto &candidate) {
        return std::memcmp(candidate.s6_addr, downstream.s6_addr, 8) == 0;
    };
    if (std::any_of(global.begin(), global.end(), conflicts) || std::any_of(ula.begin(), ula.end(), conflicts)) {
        return false;
    }
    char buffer[INET6_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET6, &downstream, buffer, sizeof(buffer)) == nullptr) {
        return false;
    }
    text = buffer;
    return true;
}
} // namespace
bool NearlinkIpv6Runtime::Set(const std::string &key, const std::string &value)
{
    auto path = "/proc/sys/net/ipv6/conf/sleip0/" + key;
    std::ifstream input(path);
    std::string old;
    if (!(input >> old)) {
        return false;
    }
    if (!settings_.count(path)) {
        settings_[path] = old;
    }
    std::ofstream output(path);
    output << value;
    output.flush();
    return bool(output);
}
bool NearlinkIpv6Runtime::AddAddress(const std::string &address)
{
    int32_t ret = NetsysController::GetInstance().AddInterfaceAddress(IFACE, address, 64);
    if (ret != 0) {
        return false; // Do not claim or later delete an address owned by someone else.
    }
    addresses_.push_back(address);
    return true;
}
bool NearlinkIpv6Runtime::SetToken()
{
    // sleip0 is newly created for this context and starts with the unspecified token.
    // RTM_NEWLINK / AF_SPEC / AF_INET6 / TOKEN is the kernel's standard TUN SLAAC path.
    struct Request {
        nlmsghdr header;
        ifinfomsg info;
        char attributes[128];
    } request{};
    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(ifinfomsg));
    request.header.nlmsg_type = RTM_NEWLINK;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    request.header.nlmsg_seq = 1;
    request.info.ifi_family = AF_UNSPEC;
    request.info.ifi_index = ifindex_;
    auto attribute = [&request](uint16_t type, const void *data, size_t length) {
        auto *attr =
            reinterpret_cast<rtattr *>(reinterpret_cast<char *>(&request) + NLMSG_ALIGN(request.header.nlmsg_len));
        attr->rta_type = type;
        attr->rta_len = RTA_LENGTH(length);
        if (length) {
            memcpy(RTA_DATA(attr), data, length);
        }
        request.header.nlmsg_len = NLMSG_ALIGN(request.header.nlmsg_len) + RTA_ALIGN(attr->rta_len);
        return attr;
    };
    auto *afSpec = attribute(IFLA_AF_SPEC, nullptr, 0);
    auto *af = attribute(AF_INET6, nullptr, 0);
    in6_addr token{};
    if (!tokenOwned_ && !Layer2Token(layer2_, token)) {
        return false;
    }
    attribute(IFLA_INET6_TOKEN, &token, sizeof(token));
    af->rta_len = reinterpret_cast<char *>(&request) + request.header.nlmsg_len - reinterpret_cast<char *>(af);
    afSpec->rta_len = reinterpret_cast<char *>(&request) + request.header.nlmsg_len - reinterpret_cast<char *>(afSpec);
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) {
        return false;
    }
    timeval timeout{1, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_nl kernel{};
    kernel.nl_family = AF_NETLINK;
    bool ok = sendto(fd, &request, request.header.nlmsg_len, 0, reinterpret_cast<sockaddr *>(&kernel),
                     sizeof(kernel)) == static_cast<ssize_t>(request.header.nlmsg_len);
    char reply[4096]{};
    ssize_t size = ok ? recv(fd, reply, sizeof(reply), 0) : -1;
    auto *header = reinterpret_cast<nlmsghdr *>(reply);
    ok = size >= static_cast<ssize_t>(NLMSG_LENGTH(sizeof(nlmsgerr))) && header->nlmsg_type == NLMSG_ERROR &&
         reinterpret_cast<nlmsgerr *>(NLMSG_DATA(header))->error == 0;
    close(fd);
    if (ok) {
        tokenOwned_ = !tokenOwned_;
    }
    return ok;
}
bool NearlinkIpv6Runtime::Prepare(bool gateway, const std::string &layer2)
{
    if (prepared_) {
        return ifindex_ == if_nametoindex(IFACE);
    }
    if (ifindex_ && !Cleanup()) {
        return false;
    }
    in6_addr token{};
    if (!Layer2Token(layer2, token)) {
        return false;
    }
    ifindex_ = if_nametoindex(IFACE);
    layer2_ = layer2;
    if (!ifindex_) {
        return false;
    }
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    ifreq request{};
    memcpy(request.ifr_name, IFACE, sizeof("sleip0"));
    bool ok = ioctl(fd, SIOCGIFFLAGS, &request) == 0;
    if (ok) {
        flags_ = request.ifr_flags;
        flagsOwned_ = true;
        request.ifr_flags = (flags_ | IFF_UP | IFF_MULTICAST) & ~IFF_NOARP;
        ok = ioctl(fd, SIOCSIFFLAGS, &request) == 0;
    }
    close(fd);
    if (!ok || !Set("disable_ipv6", "0") || !Set("forwarding", gateway ? "1" : "0") ||
        !Set("accept_ra", gateway ? "0" : "2") || !Set("autoconf", gateway ? "0" : "1") || !Set("use_tempaddr", "0") ||
        !Set("accept_dad", "1") || !Set("dad_transmits", "1")) {
        return false;
    }
    if (!gateway && (!SetToken() || !Set("router_solicitations", "6") || !Set("router_solicitation_interval", "1"))) {
        return false;
    }
    prepared_ = AddAddress(gateway ? "fe80::1" : "fe80::2");
    return prepared_;
}
bool NearlinkIpv6Runtime::Advertise(const NetLinkInfo *upstream, bool forwarding, bool dnsReady)
{
    auto now = std::chrono::steady_clock::now();
    ExpireRetiredPrefixes(now);
    std::string prefix, dns;
    in6_addr binary{}, resolver{}, token{};
    bool configured = DeriveDownstreamPrefix(upstream, binary, prefix) && Layer2Token(layer2_, token);
    if (configured) {
        resolver = binary;
        std::copy(token.s6_addr + 8, token.s6_addr + 16, resolver.s6_addr + 8);
        char defaultDns[INET6_ADDRSTRLEN]{};
        configured = inet_ntop(AF_INET6, &resolver, defaultDns, sizeof(defaultDns)) != nullptr;
        if (configured) {
            dns = defaultDns;
        }
    }
    bool defaultRoute = false;
    if (upstream != nullptr) {
        for (const auto &r : upstream->routeList_) {
            defaultRoute = defaultRoute || (r.destination_.family_ == AF_INET6 && r.destination_.prefixlen_ == 0);
        }
    }
    if (!configured) {
        prefix.clear();
        dns.clear();
    }
    bool changed = prefix != prefix_ || dns != dns_;
    configured = SelectPrefix(prefix, dns, configured, now);
    if (!daemon_ && (!prefix_.empty() || !retired_.empty())) {
        daemon_ = std::make_shared<RouterAdvertisementDaemon>();
        if (daemon_->Init(IFACE) != 0) {
            daemon_.reset();
            return false;
        }
    }
    if (!daemon_) {
        return false;
    }
    RaParams params;
    params.layer3_ = true;
    params.macAddr_ = layer2_;
    params.mtu_ = 1500;
    params.routerLifetime_ = configured && forwarding && defaultRoute ? 180 : 0;
    if (!dnsReady) {
        dns.clear();
    }
    params.rdnssLifetime_ = configured && dnsReady ? 180 : 0;
    if (configured) {
        IpPrefix p;
        Prefix(prefix_, p.prefix);
        p.prefixesLength = 64;
        p.validLifetime = PREFIX_VALID_SECONDS;
        p.preferredLifetime = PREFIX_PREFERRED_SECONDS;
        params.prefixes_.push_back(p);
        if (dnsReady) {
            params.dnses_.push_back(resolver);
        }
    }
    for (const auto &old : retired_) {
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(old.until - now).count();
        IpPrefix p;
        Prefix(old.prefix, p.prefix);
        p.prefixesLength = 64;
        p.preferredLifetime = 0;
        p.validLifetime = remaining > 0 ? remaining : 0;
        params.prefixes_.push_back(p);
    }
    PublishAdvertisement(params, dns, changed);
    return configured && ReconcileGatewayAddress(now);
}

void NearlinkIpv6Runtime::PublishAdvertisement(const RaParams &params, const std::string &dns, bool changed)
{
    if (dns != dns_ && !dns_.empty() && raStarted_) {
        // Explicitly remove the old DNS source before advertising its replacement.
        RaParams withdraw = params;
        withdraw.dnses_.clear();
        in6_addr old{};
        inet_pton(AF_INET6, dns_.c_str(), &old);
        withdraw.dnses_.push_back(old);
        withdraw.rdnssLifetime_ = 0;
        daemon_->BuildNewRa(withdraw);
        (void)daemon_->AdvertiseNow();
    }
    daemon_->BuildNewRa(params);
    if (!raStarted_) {
        raStarted_ = daemon_->StartRa() == 0;
    }
    if (raStarted_ && (changed || lastRouterLifetime_ != params.routerLifetime_)) {
        (void)daemon_->AdvertiseNow();
    }
    lastRouterLifetime_ = params.routerLifetime_;
    dns_ = dns;
}

bool NearlinkIpv6Runtime::SelectPrefix(std::string &prefix, std::string &dns, bool configured,
                                       std::chrono::steady_clock::time_point now)
{
    if (prefix != prefix_ && !prefix_.empty()) {
        retired_.push_back({prefix_, gateway_, now + std::chrono::seconds(PREFIX_VALID_SECONDS), routeOwned_});
        prefix_.clear();
        gateway_.clear();
        routeOwned_ = false;
        gatewayAddressOwned_ = false;
    }
    if (prefix_.empty() && configured) {
        // A retired prefix can become active again without double-owning its resources.
        for (auto it = retired_.begin(); it != retired_.end(); ++it) {
            if (it->prefix == prefix) {
                gateway_ = it->gateway;
                routeOwned_ = it->route;
                gatewayAddressOwned_ = std::find(addresses_.begin(), addresses_.end(), gateway_) != addresses_.end();
                retired_.erase(it);
                break;
            }
        }
        // The active prefix plus retained prefixes share four slots. When full,
        // withdraw the old router/DNS immediately and retry admission after cleanup.
        if (retired_.size() >= MAX_PREFIXES) {
            configured = false;
            prefix.clear();
            dns.clear();
        } else {
            prefix_ = prefix;
            gateway_ = dns;
            advertisedAt_ = now;
        }
    }
    return configured;
}

void NearlinkIpv6Runtime::ExpireRetiredPrefixes(std::chrono::steady_clock::time_point now)
{
    for (auto it = retired_.begin(); it != retired_.end();) {
        if (it->until > now) {
            ++it;
            continue;
        }
        bool released = true;
        if (it->route) {
            if (RouteRemoved(NetsysController::GetInstance().NetworkRemoveRoute(LOCAL_NETWORK_ID, IFACE,
                                                                                it->prefix + "/64", "::"))) {
                it->route = false;
            } else {
                released = false;
            }
        }
        if (std::find(addresses_.begin(), addresses_.end(), it->gateway) != addresses_.end()) {
            if (AddressRemoved(NetsysController::GetInstance().DelInterfaceAddress(IFACE, it->gateway, 64))) {
                addresses_.erase(std::remove(addresses_.begin(), addresses_.end(), it->gateway), addresses_.end());
            } else {
                released = false;
            }
        }
        if (released) {
            it = retired_.erase(it);
        } else {
            ++it;
        }
    }
}

bool NearlinkIpv6Runtime::ReconcileGatewayAddress(std::chrono::steady_clock::time_point now)
{
    // Prefix authorization must precede gateway address DAD. Never block the shared worker to wait.
    if (raStarted_ && !gatewayAddressOwned_ && now - advertisedAt_ >= std::chrono::seconds(5)) {
        gatewayAddressOwned_ = AddAddress(gateway_);
    }
    if (gatewayAddressOwned_ && !routeOwned_) {
        routeOwned_ =
            NetsysController::GetInstance().NetworkAddRoute(LOCAL_NETWORK_ID, IFACE, prefix_ + "/64", "::") == 0;
    }
    bool usable = false;
    std::ifstream kernel("/proc/net/if_inet6");
    std::string hex, iface;
    unsigned index, length, scope, flags;
    in6_addr expected{};
    inet_pton(AF_INET6, gateway_.c_str(), &expected);
    char expectedHex[33]{};
    for (unsigned i = 0; i < 16; ++i) {
        snprintf(expectedHex + i * 2, 3, "%02x", expected.s6_addr[i]);
    }
    while (kernel >> hex >> std::hex >> index >> length >> scope >> flags >> iface) {
        if (iface == IFACE && index == ifindex_ && hex == expectedHex && !(flags & UNUSABLE_ADDRESS_FLAGS)) {
            usable = true;
        }
    }
    return !prefix_.empty() && raStarted_ && gatewayAddressOwned_ && routeOwned_ && usable;
}
bool NearlinkIpv6Runtime::Cleanup()
{
    if (daemon_) {
        daemon_->StopRa();
        daemon_.reset();
    }
    if (ifindex_ != if_nametoindex(IFACE)) {
        *this = NearlinkIpv6Runtime{};
        return true;
    }
    bool ok = true;
    for (auto &old : retired_) {
        if (old.route) {
            if (RouteRemoved(NetsysController::GetInstance().NetworkRemoveRoute(LOCAL_NETWORK_ID, IFACE,
                                                                                old.prefix + "/64", "::"))) {
                old.route = false;
            } else {
                ok = false;
            }
        }
    }
    if (routeOwned_) {
        if (RouteRemoved(
                NetsysController::GetInstance().NetworkRemoveRoute(LOCAL_NETWORK_ID, IFACE, prefix_ + "/64", "::"))) {
            routeOwned_ = false;
        } else {
            ok = false;
        }
    }
    for (auto it = addresses_.begin(); it != addresses_.end();) {
        auto ret = NetsysController::GetInstance().DelInterfaceAddress(IFACE, *it, 64);
        if (AddressRemoved(ret)) {
            it = addresses_.erase(it);
        } else {
            ok = false;
            ++it;
        }
    }
    if (tokenOwned_ && !SetToken()) {
        ok = false;
    }
    for (auto it = settings_.begin(); it != settings_.end();) {
        std::ofstream output(it->first);
        output << it->second;
        output.flush();
        if (output) {
            it = settings_.erase(it);
        } else {
            ok = false;
            ++it;
        }
    }
    if (flagsOwned_) {
        int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        ifreq request{};
        memcpy(request.ifr_name, IFACE, sizeof("sleip0"));
        request.ifr_flags = flags_;
        if (fd >= 0 && ioctl(fd, SIOCSIFFLAGS, &request) == 0) {
            flagsOwned_ = false;
        } else {
            ok = false;
        }
        if (fd >= 0) {
            close(fd);
        }
    }
    if (ok) {
        *this = NearlinkIpv6Runtime{};
    }
    return ok;
}
} // namespace OHOS::NetManagerStandard
