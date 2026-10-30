/* Copyright (c) 2026 Huawei Device Co., Ltd. Licensed under the Apache License, Version 2.0. */
#include "nearlink_upstream_signal.h"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <net/if.h>
#include <new>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include "fwmark.h"
#include "netmgr_ext_log_wrapper.h"

namespace OHOS::NetManagerStandard {
namespace {
using Clock = std::chrono::steady_clock;
constexpr uint16_t SIGNAL_PORT = 30201;
constexpr size_t FRAME_SIZE = 32;
constexpr uint8_t QUERY = 1, STATUS = 2, ACK = 3;
constexpr auto RETRY = std::chrono::seconds(1);
constexpr auto REFRESH = std::chrono::seconds(5);
constexpr auto EXPIRY = std::chrono::seconds(15);
using Token = std::array<uint8_t, 16>;
using Frame = std::array<uint8_t, FRAME_SIZE>;

bool BindNetwork(int fd, int32_t netId)
{
    // netmanager configures its own UDP fd with existing CAP_NET_ADMIN/self setopt.
    // Reuse the Base mark fields used by the ordinary network-binding path.
    nmd::Fwmark mark;
    socklen_t size = sizeof(mark.intValue);
    if (getsockopt(fd, SOL_SOCKET, SO_MARK, &mark.intValue, &size) != 0) {
        return false;
    }
    mark.netId = static_cast<uint16_t>(netId);
    mark.explicitlySelected = true;
    mark.protectedFromVpn = true;
    return setsockopt(fd, SOL_SOCKET, SO_MARK, &mark.intValue, sizeof(mark.intValue)) == 0;
}

Frame Encode(uint8_t kind, uint8_t mask, const Token &token, uint64_t sequence)
{
    Frame frame{'N', 'L', 'U', '1', kind, mask};
    std::copy(token.begin(), token.end(), frame.begin() + 8);
    for (size_t i = 0; i < 8; ++i) {
        frame[31 - i] = static_cast<uint8_t>(sequence >> (i * 8));
    }
    return frame;
}

bool Decode(const Frame &frame, uint8_t &kind, uint8_t &mask, Token &token, uint64_t &sequence)
{
    if (memcmp(frame.data(), "NLU1", 4) != 0 || frame[6] || frame[7] || frame[5] > 3) {
        return false;
    }
    kind = frame[4];
    mask = frame[5];
    std::copy(frame.begin() + 8, frame.begin() + 24, token.begin());
    sequence = 0;
    for (size_t i = 24; i < frame.size(); ++i) {
        sequence = (sequence << 8) | frame[i];
    }
    return std::any_of(token.begin(), token.end(), [](uint8_t byte) { return byte != 0; }) &&
           ((kind == QUERY && mask == 0 && sequence == 0) || (kind == STATUS && sequence != 0) ||
            (kind == ACK && mask == 0 && sequence != 0));
}

bool SameEndpoint(const sockaddr_storage &a, const sockaddr_storage &b)
{
    if (a.ss_family != b.ss_family) {
        return false;
    }
    if (a.ss_family == AF_INET) {
        const auto &first = reinterpret_cast<const sockaddr_in &>(a);
        const auto &second = reinterpret_cast<const sockaddr_in &>(b);
        return first.sin_port == second.sin_port && first.sin_addr.s_addr == second.sin_addr.s_addr;
    }
    const auto &first = reinterpret_cast<const sockaddr_in6 &>(a);
    const auto &second = reinterpret_cast<const sockaddr_in6 &>(b);
    return first.sin6_port == second.sin6_port && first.sin6_scope_id == second.sin6_scope_id &&
           memcmp(&first.sin6_addr, &second.sin6_addr, sizeof(first.sin6_addr)) == 0;
}

bool Endpoint(sockaddr_storage &out, const std::string &address, bool ipv6, uint16_t port, uint32_t index)
{
    out = {};
    if (ipv6) {
        auto &value = reinterpret_cast<sockaddr_in6 &>(out);
        value.sin6_family = AF_INET6;
        value.sin6_port = htons(port);
        value.sin6_scope_id = index;
        return inet_pton(AF_INET6, address.c_str(), &value.sin6_addr) == 1;
    }
    auto &value = reinterpret_cast<sockaddr_in &>(out);
    value.sin_family = AF_INET;
    value.sin_port = htons(port);
    return inet_pton(AF_INET, address.c_str(), &value.sin_addr) == 1;
}
} // namespace

struct NearlinkUpstreamSignal::Impl {
    struct Socket {
        int fd{-1};
        sockaddr_storage peer{};
        socklen_t size{0};
        Token token{};
        uint64_t acknowledged{0};
        Clock::time_point seen{}, sent{}, queried{}, openRetry{};
    };
    Link link;
    bool gateway{false};
    std::array<Socket, 2> sockets;
    Token token{};
    State state;
    uint64_t sequence{static_cast<uint64_t>(Clock::now().time_since_epoch().count()) | 1};
    uint64_t receivedSequence{0};
    Clock::time_point lastReply{};
    int lastError{0};

    ~Impl()
    {
        for (auto &socket : sockets) {
            if (socket.fd >= 0) {
                close(socket.fd);
            }
        }
    }

    bool Matches(const Link &other, bool server) const
    {
        return gateway == server && link.iface == other.iface && link.ifindex == other.ifindex &&
               link.netId == other.netId && link.localIpv4 == other.localIpv4 &&
               link.gatewayIpv4 == other.gatewayIpv4 && link.ipv6 == other.ipv6;
    }

    void Open(Socket &entry, bool ipv6, Clock::time_point now)
    {
        if (entry.fd >= 0 || now < entry.openRetry || (ipv6 ? !link.ipv6 : link.localIpv4.empty())) {
            return;
        }
        entry.openRetry = now + REFRESH;
        sockaddr_storage local{}, remote{};
        const std::string address = ipv6 ? (gateway ? "fe80::1" : "fe80::2") : link.localIpv4;
        const std::string target = ipv6 ? "fe80::1" : link.gatewayIpv4;
        if (!Endpoint(local, address, ipv6, gateway ? SIGNAL_PORT : 0, link.ifindex) ||
            (!gateway && !Endpoint(remote, target, ipv6, SIGNAL_PORT, link.ifindex))) {
            return;
        }
        socklen_t size = ipv6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
        int fd = socket(ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return;
        }
        int one = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, link.iface.c_str(), link.iface.size() + 1) != 0 ||
            (ipv6 && setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one)) != 0) ||
            !BindNetwork(fd, link.netId) ||
            bind(fd, reinterpret_cast<const sockaddr *>(&local), size) != 0) {
            close(fd);
            return;
        }
        entry.fd = fd;
        entry.size = size;
        if (!gateway) {
            entry.peer = remote;
        }
    }

    void Send(Socket &entry, uint8_t kind, Clock::time_point now)
    {
        if (entry.fd < 0 || (gateway && entry.peer.ss_family == 0)) {
            return;
        }
        const auto frame = Encode(kind, kind == STATUS ? state.available : 0, gateway ? entry.token : token,
                                  kind == QUERY ? 0 : (gateway ? sequence : receivedSequence));
        if (sendto(entry.fd, frame.data(), frame.size(), MSG_DONTWAIT, reinterpret_cast<const sockaddr *>(&entry.peer),
                   entry.size) == static_cast<ssize_t>(frame.size())) {
            if (kind == QUERY) {
                entry.queried = now;
            } else if (kind == STATUS) {
                entry.sent = now;
            }
        }
    }

    void HandleGateway(Socket &entry, const sockaddr_storage &source, uint8_t kind, const Token &nonce,
                       uint64_t revision, Clock::time_point now)
    {
        if (kind == QUERY) {
            bool changed = !SameEndpoint(source, entry.peer) || nonce != entry.token;
            entry.peer = source;
            entry.token = nonce;
            entry.seen = now;
            if (changed) {
                entry.acknowledged = 0;
            }
            if (changed || now - entry.sent >= RETRY) {
                Send(entry, STATUS, now);
            }
        } else if (kind == ACK && SameEndpoint(source, entry.peer) && nonce == entry.token && revision == sequence) {
            entry.acknowledged = revision;
            entry.seen = now;
        }
    }

    void HandleTerminal(Socket &entry, const sockaddr_storage &source, uint8_t kind, uint8_t mask, const Token &nonce,
                        uint64_t revision, Clock::time_point now)
    {
        if (kind == STATUS && SameEndpoint(source, entry.peer) && nonce == token && revision >= receivedSequence &&
            (revision != receivedSequence || mask == state.available)) {
            receivedSequence = revision;
            state.changed |= state.known ? state.available ^ mask : 0;
            state.known = true;
            state.available = mask;
            lastReply = now;
            Send(entry, ACK, now);
        }
    }

    void Receive(Socket &entry, Clock::time_point now)
    {
        // Bound work even if a peer floods this private port. Truncated/oversized frames are rejected.
        for (unsigned count = 0; entry.fd >= 0 && count < 8; ++count) {
            std::array<uint8_t, FRAME_SIZE + 1> bytes{};
            sockaddr_storage source{};
            socklen_t size = sizeof(source);
            auto received = recvfrom(entry.fd, bytes.data(), bytes.size(), MSG_DONTWAIT,
                                     reinterpret_cast<sockaddr *>(&source), &size);
            if (received < 0) {
                break;
            }
            Frame frame{};
            Token nonce{};
            uint64_t revision = 0;
            uint8_t kind = 0, mask = 0;
            std::copy_n(bytes.begin(), frame.size(), frame.begin());
            if (received != FRAME_SIZE || size != entry.size || !Decode(frame, kind, mask, nonce, revision)) {
                continue;
            }
            if (gateway) {
                HandleGateway(entry, source, kind, nonce, revision, now);
            } else {
                HandleTerminal(entry, source, kind, mask, nonce, revision, now);
            }
        }
    }
};

NearlinkUpstreamSignal::NearlinkUpstreamSignal() = default;
NearlinkUpstreamSignal::~NearlinkUpstreamSignal() = default;

void NearlinkUpstreamSignal::Reset()
{
    impl_.reset();
}

bool NearlinkUpstreamSignal::Prepare(const Link &link, bool gateway)
{
    if (link.iface.empty() || link.iface.size() >= IFNAMSIZ || link.ifindex == 0 || link.netId < 0 ||
        static_cast<uint32_t>(link.netId) > nmd::FWMARK_NET_ID_MASK ||
        if_nametoindex(link.iface.c_str()) != link.ifindex) {
        Reset();
        return false;
    }
    if (impl_ && !impl_->Matches(link, gateway)) {
        Reset();
    }
    if (!impl_) {
        impl_.reset(new (std::nothrow) Impl());
        if (!impl_) {
            return false;
        }
        impl_->link = link;
        impl_->gateway = gateway;
        if (!gateway && getrandom(impl_->token.data(), impl_->token.size(), GRND_NONBLOCK) !=
                            static_cast<ssize_t>(impl_->token.size())) {
            Reset();
            return false;
        }
    }
    auto now = Clock::now();
    impl_->Open(impl_->sockets[0], false, now);
    impl_->Open(impl_->sockets[1], true, now);
    bool opened = impl_->sockets[0].fd >= 0 || impl_->sockets[1].fd >= 0;
    if (!opened && errno != impl_->lastError) {
        impl_->lastError = errno;
        NETMGR_EXT_LOG_W("[NearlinkIpShare][UpstreamSignal] socket unavailable iface=%{public}s errno=%{public}d",
                         link.iface.c_str(), errno);
    }
    return opened;
}

void NearlinkUpstreamSignal::Gateway(const Link &link, uint8_t available)
{
    if (!Prepare(link, true)) {
        return;
    }
    auto now = Clock::now();
    if (impl_->state.available != available) {
        impl_->state.available = available & 3;
        ++impl_->sequence;
        for (auto &entry : impl_->sockets) {
            impl_->Send(entry, STATUS, now);
        }
    }
    for (auto &entry : impl_->sockets) {
        impl_->Receive(entry, now);
        if (entry.peer.ss_family != 0 && now - entry.seen > EXPIRY) {
            entry.peer = {};
        }
        if (entry.acknowledged != impl_->sequence && now - entry.sent >= RETRY) {
            impl_->Send(entry, STATUS, now);
        }
    }
}

NearlinkUpstreamSignal::State NearlinkUpstreamSignal::Terminal(const Link &link)
{
    if (!Prepare(link, false)) {
        return {};
    }
    auto now = Clock::now();
    impl_->state.changed = 0;
    if (impl_->state.known && now - impl_->lastReply > EXPIRY) {
        Reset(); // A new challenge rejects delayed frames and permits legacy-probe fallback.
        return {};
    }
    for (auto &entry : impl_->sockets) {
        impl_->Receive(entry, now);
        if (now - entry.queried >= (impl_->state.known ? REFRESH : RETRY)) {
            impl_->Send(entry, QUERY, now);
        }
    }
    return impl_->state;
}
} // namespace OHOS::NetManagerStandard
