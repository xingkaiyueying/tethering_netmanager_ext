/* Copyright (c) 2026 Huawei Device Co., Ltd. Licensed under the Apache License, Version 2.0. */
#include "nearlink_ipv6_runtime.h"
#include "netsys_controller.h"
#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <linux/if_link.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace OHOS::NetManagerStandard {
namespace {
constexpr const char *IFACE = "sleip0";
bool Prefix(const std::string &text, in6_addr &address)
{
    if (inet_pton(AF_INET6, text.c_str(), &address) != 1 ||
        ((address.s6_addr[0] & 0xfe) != 0xfc && (address.s6_addr[0] & 0xe0) != 0x20)) return false;
    for (size_t i = 8; i < 16; ++i) if (address.s6_addr[i]) return false;
    return true;
}
}
bool NearlinkIpv6Runtime::Set(const std::string &key, const std::string &value)
{
    auto path = "/proc/sys/net/ipv6/conf/sleip0/" + key;
    std::ifstream input(path);
    std::string old;
    if (!(input >> old)) return false;
    if (!settings_.count(path)) settings_[path] = old;
    std::ofstream output(path);
    output << value; output.flush();
    return bool(output);
}
bool NearlinkIpv6Runtime::AddAddress(const std::string &address)
{
    int32_t ret = NetsysController::GetInstance().AddInterfaceAddress(IFACE, address, 64);
    if (ret != 0) return false; // Do not claim or later delete an address owned by someone else.
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
    request.info.ifi_family = AF_UNSPEC; request.info.ifi_index = ifindex_;
    auto attribute = [&request](uint16_t type, const void *data, size_t length) {
        auto *attr = reinterpret_cast<rtattr *>(reinterpret_cast<char *>(&request) +
            NLMSG_ALIGN(request.header.nlmsg_len));
        attr->rta_type = type; attr->rta_len = RTA_LENGTH(length);
        if (length) memcpy(RTA_DATA(attr), data, length);
        request.header.nlmsg_len = NLMSG_ALIGN(request.header.nlmsg_len) + RTA_ALIGN(attr->rta_len);
        return attr;
    };
    auto *afSpec = attribute(IFLA_AF_SPEC, nullptr, 0);
    auto *af = attribute(AF_INET6, nullptr, 0);
    in6_addr token{};
    if (!tokenOwned_) token.s6_addr[15] = 2;
    attribute(IFLA_INET6_TOKEN, &token, sizeof(token));
    af->rta_len = reinterpret_cast<char *>(&request) + request.header.nlmsg_len - reinterpret_cast<char *>(af);
    afSpec->rta_len = reinterpret_cast<char *>(&request) + request.header.nlmsg_len - reinterpret_cast<char *>(afSpec);
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return false;
    timeval timeout{1, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_nl kernel{}; kernel.nl_family = AF_NETLINK;
    bool ok = sendto(fd, &request, request.header.nlmsg_len, 0, reinterpret_cast<sockaddr *>(&kernel),
        sizeof(kernel)) == static_cast<ssize_t>(request.header.nlmsg_len);
    char reply[4096]{};
    ssize_t size = ok ? recv(fd, reply, sizeof(reply), 0) : -1;
    auto *header = reinterpret_cast<nlmsghdr *>(reply);
    ok = size >= static_cast<ssize_t>(NLMSG_LENGTH(sizeof(nlmsgerr))) && header->nlmsg_type == NLMSG_ERROR &&
        reinterpret_cast<nlmsgerr *>(NLMSG_DATA(header))->error == 0;
    close(fd);
    if (ok) tokenOwned_ = !tokenOwned_;
    return ok;
}
bool NearlinkIpv6Runtime::Prepare(bool gateway, const std::string &layer2)
{
    if (prepared_) return ifindex_ == if_nametoindex(IFACE);
    if (ifindex_ && !Cleanup()) return false;
    ifindex_ = if_nametoindex(IFACE); layer2_ = layer2;
    if (!ifindex_) return false;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    ifreq request{}; memcpy(request.ifr_name, IFACE, sizeof("sleip0"));
    bool ok = ioctl(fd, SIOCGIFFLAGS, &request) == 0;
    if (ok) {
        flags_ = request.ifr_flags; flagsOwned_ = true;
        request.ifr_flags = (flags_ | IFF_UP | IFF_MULTICAST) & ~IFF_NOARP;
        ok = ioctl(fd, SIOCSIFFLAGS, &request) == 0;
    }
    close(fd);
    if (!ok || !Set("disable_ipv6", "0") || !Set("forwarding", gateway ? "1" : "0") ||
        !Set("accept_ra", gateway ? "0" : "2") || !Set("autoconf", gateway ? "0" : "1") ||
        !Set("use_tempaddr", "0") || !Set("accept_dad", "1") || !Set("dad_transmits", "1")) return false;
    if (!gateway && (!SetToken() || !Set("router_solicitations", "6") ||
        !Set("router_solicitation_interval", "1"))) return false;
    prepared_ = AddAddress(gateway ? "fe80::1" : "fe80::2");
    return prepared_;
}
bool NearlinkIpv6Runtime::Advertise(const NetLinkInfo *upstream, bool forwarding)
{
    auto now = std::chrono::steady_clock::now();
    // Read the shared configuration on reconciliation so renumbering remains live.
    std::ifstream file("/system/etc/communication/netmanager_ext/network_share_config.cfg");
    std::string prefix, dns, line;
    bool prefixSeen = false, dnsSeen = false, duplicate = false;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto separator = line.find(':'); // IPv6 colons belong to the value.
        if (separator == std::string::npos) continue;
        auto key = line.substr(0, separator);
        auto value = line.substr(separator + 1);
        if (key == "nearlink_ipv6_prefix") {
            duplicate |= prefixSeen; prefixSeen = true; prefix = value;
        } else if (key == "nearlink_ipv6_dns") {
            duplicate |= dnsSeen; dnsSeen = true; dns = value;
        }
    }
    in6_addr binary{}, resolver{};
    bool configured = !duplicate && prefixSeen && dnsSeen && Prefix(prefix, binary) &&
        inet_pton(AF_INET6, dns.c_str(), &resolver) == 1 && !IN6_IS_ADDR_UNSPECIFIED(&resolver) &&
        !IN6_IS_ADDR_MULTICAST(&resolver) && !IN6_IS_ADDR_LINKLOCAL(&resolver);
    bool defaultRoute = false;
    if (upstream && configured) {
        for (const auto &a : upstream->netAddrList_) {
            in6_addr onLink{};
            if (a.family_ != AF_INET6 || inet_pton(AF_INET6, a.address_.c_str(), &onLink) != 1) continue;
            unsigned bits = std::min<unsigned>(a.prefixlen_, 64);
            bool overlaps = true;
            for (unsigned bit = 0; bit < bits; ++bit) {
                unsigned mask = 0x80 >> (bit % 8);
                if ((onLink.s6_addr[bit / 8] & mask) != (binary.s6_addr[bit / 8] & mask)) overlaps = false;
            }
            if (overlaps) configured = false;
        }
        for (const auto &r : upstream->routeList_)
            defaultRoute = defaultRoute || (r.destination_.family_ == AF_INET6 && r.destination_.prefixlen_ == 0);
    }
    if (!configured) { prefix.clear(); dns.clear(); }
    bool changed = prefix != prefix_ || dns != dns_;
    if (prefix != prefix_ && !prefix_.empty()) {
        if (retired_.size() >= 3) return false; // At most four advertised/owned prefixes.
        retired_.push_back({prefix_, gateway_, now + std::chrono::seconds(300), routeOwned_});
        prefix_.clear(); gateway_.clear(); routeOwned_ = false; gatewayAddressOwned_ = false;
    }
    if (prefix_.empty() && configured) {
        // A retired prefix can become active again without double-owning its resources.
        for (auto it = retired_.begin(); it != retired_.end(); ++it) {
            if (it->prefix == prefix) {
                gateway_ = it->gateway; routeOwned_ = it->route;
                gatewayAddressOwned_ = std::find(addresses_.begin(), addresses_.end(), gateway_) != addresses_.end();
                retired_.erase(it); break;
            }
        }
        prefix_ = prefix;
        binary.s6_addr[15] = 1;
        char address[INET6_ADDRSTRLEN]{}; inet_ntop(AF_INET6, &binary, address, sizeof(address));
        gateway_ = address; advertisedAt_ = now;
    }
    if (!daemon_ && (!prefix_.empty() || !retired_.empty())) {
        daemon_ = std::make_shared<RouterAdvertisementDaemon>();
        if (daemon_->Init(IFACE) != 0) { daemon_.reset(); return false; }
    }
    if (!daemon_) return false;
    RaParams params; params.layer3_ = true; params.macAddr_ = layer2_; params.mtu_ = 1500;
    params.routerLifetime_ = configured && forwarding && defaultRoute ? 180 : 0;
    params.rdnssLifetime_ = configured ? 180 : 0;
    if (configured) {
        IpPrefix p; Prefix(prefix_, p.prefix); p.prefixesLength = 64;
        p.validLifetime = 300; p.preferredLifetime = 180; params.prefixes_.push_back(p);
        params.dnses_.push_back(resolver);
    }
    for (auto it = retired_.begin(); it != retired_.end();) {
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(it->until - now).count();
        IpPrefix p; Prefix(it->prefix, p.prefix); p.prefixesLength = 64;
        p.preferredLifetime = 0; p.validLifetime = remaining > 0 ? remaining : 0;
        params.prefixes_.push_back(p);
        if (remaining > 0) { ++it; continue; }
        bool released = true;
        if (it->route) {
            if (NetsysController::GetInstance().NetworkRemoveRoute(99, IFACE, it->prefix + "/64", "::") == 0)
                it->route = false;
            else released = false;
        }
        if (std::find(addresses_.begin(), addresses_.end(), it->gateway) != addresses_.end()) {
            if (NetsysController::GetInstance().DelInterfaceAddress(IFACE, it->gateway, 64) == 0)
                addresses_.erase(std::remove(addresses_.begin(), addresses_.end(), it->gateway), addresses_.end());
            else released = false;
        }
        if (released) it = retired_.erase(it); else ++it;
    }
    if (dns != dns_ && !dns_.empty() && raStarted_) {
        // Explicitly remove the old DNS source before advertising its replacement.
        RaParams withdraw = params; withdraw.dnses_.clear();
        in6_addr old{}; inet_pton(AF_INET6, dns_.c_str(), &old);
        withdraw.dnses_.push_back(old); withdraw.rdnssLifetime_ = 0;
        daemon_->BuildNewRa(withdraw); (void)daemon_->AdvertiseNow();
    }
    daemon_->BuildNewRa(params);
    if (!raStarted_) raStarted_ = daemon_->StartRa() == 0;
    if (raStarted_ && (changed || lastRouterLifetime_ != params.routerLifetime_))
        (void)daemon_->AdvertiseNow();
    lastRouterLifetime_ = params.routerLifetime_; dns_ = dns;
    // Prefix authorization must precede gateway address DAD. Never block the shared worker to wait.
    if (configured && raStarted_ && !gatewayAddressOwned_ && now - advertisedAt_ >= std::chrono::seconds(5)) {
        gatewayAddressOwned_ = AddAddress(gateway_);
    }
    if (gatewayAddressOwned_ && !routeOwned_)
        routeOwned_ = NetsysController::GetInstance().NetworkAddRoute(99, IFACE, prefix_ + "/64", "::") == 0;
    bool usable = false;
    std::ifstream kernel("/proc/net/if_inet6");
    std::string hex, iface; unsigned index, length, scope, flags;
    in6_addr expected{}; inet_pton(AF_INET6, gateway_.c_str(), &expected);
    char expectedHex[33]{};
    for (unsigned i = 0; i < 16; ++i) snprintf(expectedHex + i * 2, 3, "%02x", expected.s6_addr[i]);
    while (kernel >> hex >> std::hex >> index >> length >> scope >> flags >> iface) {
        if (iface == IFACE && index == ifindex_ && hex == expectedHex && !(flags & (0x04 | 0x08 | 0x40))) usable = true;
    }
    return configured && raStarted_ && gatewayAddressOwned_ && routeOwned_ && usable;
}
bool NearlinkIpv6Runtime::Cleanup()
{
    if (daemon_) { daemon_->StopRa(); daemon_.reset(); }
    if (ifindex_ != if_nametoindex(IFACE)) {
        *this = NearlinkIpv6Runtime{}; return true;
    }
    bool ok = true;
    for (auto &old : retired_) {
        if (old.route) {
            if (NetsysController::GetInstance().NetworkRemoveRoute(99, IFACE, old.prefix + "/64", "::") == 0)
                old.route = false;
            else ok = false;
        }
    }
    if (routeOwned_) {
        if (NetsysController::GetInstance().NetworkRemoveRoute(99, IFACE, prefix_ + "/64", "::") == 0) routeOwned_ = false;
        else ok = false;
    }
    for (auto it = addresses_.begin(); it != addresses_.end();) {
        auto ret = NetsysController::GetInstance().DelInterfaceAddress(IFACE, *it, 64);
        if (ret == 0 || ret == -EADDRNOTAVAIL || ret == -ENODEV) it = addresses_.erase(it);
        else { ok = false; ++it; }
    }
    if (tokenOwned_ && !SetToken()) ok = false;
    for (auto it = settings_.begin(); it != settings_.end();) {
        std::ofstream output(it->first); output << it->second; output.flush();
        if (output) it = settings_.erase(it); else { ok = false; ++it; }
    }
    if (flagsOwned_) {
        int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        ifreq request{}; memcpy(request.ifr_name, IFACE, sizeof("sleip0")); request.ifr_flags = flags_;
        if (fd >= 0 && ioctl(fd, SIOCSIFFLAGS, &request) == 0) flagsOwned_ = false; else ok = false;
        if (fd >= 0) close(fd);
    }
    if (ok) *this = NearlinkIpv6Runtime{};
    return ok;
}
} // namespace OHOS::NetManagerStandard
