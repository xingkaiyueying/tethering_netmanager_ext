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
constexpr size_t FRAME_SIZE = 36;
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

Frame Encode(uint8_t kind, uint8_t mask, const Token &token, uint64_t sequence,
             const std::string &gatewayIpv4 = {})
{
    Frame frame{'N', 'L', 'U', '2', kind, mask};
    std::copy(token.begin(), token.end(), frame.begin() + 8);
    for (size_t i = 0; i < 8; ++i) {
        frame[31 - i] = static_cast<uint8_t>(sequence >> (i * 8));
    }
    if (kind == STATUS && !gatewayIpv4.empty()) {
        (void)inet_pton(AF_INET, gatewayIpv4.c_str(), frame.data() + 32);
    }
    return frame;
}

bool Decode(const Frame &frame, uint8_t &kind, uint8_t &mask, Token &token, uint64_t &sequence)
{
    if (memcmp(frame.data(), "NLU2", 4) != 0 || frame[6] || frame[7] || frame[5] > 3) {
        return false;
    }
    kind = frame[4];
    mask = frame[5];
    std::copy(frame.begin() + 8, frame.begin() + 24, token.begin());
    sequence = 0;
    for (size_t i = 24; i < 32; ++i) {
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
    struct Subscriber {
        uint64_t generation{0};
        sockaddr_storage peer{};
        Token token{};
        uint64_t acknowledged{0};
        Clock::time_point seen{}, sent{};
    };
    struct Socket {
        int fd{-1};
        sockaddr_storage peer{};
        socklen_t size{0};
        Token token{};
        uint64_t acknowledged{0};
        Clock::time_point seen{}, sent{}, queried{}, openRetry{};
        std::vector<Subscriber> subscribers;
    };
    Link link;
    bool gateway{false};
    bool configurationChanged{false};
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
               link.netId == other.netId && (server || link.localIpv4 == other.localIpv4) &&
               link.gatewayIpv4 == other.gatewayIpv4 && link.ipv6 == other.ipv6 &&
               link.localIpv6 == other.localIpv6 && link.gatewayIpv6 == other.gatewayIpv6;
    }

    void Open(Socket &entry, bool ipv6, Clock::time_point now)
    {
        if (entry.fd >= 0 || now < entry.openRetry || (ipv6 ? !link.ipv6 : link.localIpv4.empty())) {
            return;
        }
        entry.openRetry = now + REFRESH;
        sockaddr_storage local{}, remote{};
        const std::string address = ipv6 ? link.localIpv6 : (gateway ? "0.0.0.0" : link.localIpv4);
        const std::string target = ipv6 ? link.gatewayIpv6 : link.gatewayIpv4;
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

    uint64_t MemberGeneration(const sockaddr_storage &source) const
    {
        for (const auto &member : link.members) {
            std::vector<std::string> addresses = source.ss_family == AF_INET ?
                std::vector<std::string>{member.ipv4} : member.ipv6;
            for (const auto &text : addresses) {
                sockaddr_storage allowed{};
                if (!Endpoint(allowed, text, source.ss_family == AF_INET6, 0, link.ifindex)) {
                    continue;
                }
                if (source.ss_family == AF_INET) {
                    if (reinterpret_cast<const sockaddr_in &>(allowed).sin_addr.s_addr ==
                        reinterpret_cast<const sockaddr_in &>(source).sin_addr.s_addr) {
                        return member.generation;
                    }
                } else if (memcmp(&reinterpret_cast<const sockaddr_in6 &>(allowed).sin6_addr,
                                  &reinterpret_cast<const sockaddr_in6 &>(source).sin6_addr, sizeof(in6_addr)) == 0 &&
                           reinterpret_cast<const sockaddr_in6 &>(source).sin6_scope_id == link.ifindex) {
                    return member.generation;
                }
            }
        }
        return 0;
    }

    void SendStatus(Socket &entry, Subscriber &subscriber, Clock::time_point now)
    {
        const auto frame = Encode(STATUS, state.available, subscriber.token, sequence, link.localIpv4);
        if (sendto(entry.fd, frame.data(), frame.size(), MSG_DONTWAIT,
                   reinterpret_cast<const sockaddr *>(&subscriber.peer), entry.size) ==
            static_cast<ssize_t>(frame.size())) {
            subscriber.sent = now;
        }
    }

    void HandleGateway(Socket &entry, const sockaddr_storage &source, uint8_t kind, const Token &nonce,
                       uint64_t revision, Clock::time_point now)
    {
        uint64_t generation = MemberGeneration(source);
        if (!generation) {
            return;
        }
        auto found = std::find_if(entry.subscribers.begin(), entry.subscribers.end(), [&](const auto &subscriber) {
            return subscriber.generation == generation && SameEndpoint(source, subscriber.peer) &&
                   nonce == subscriber.token;
        });
        if (kind == QUERY) {
            if (found == entry.subscribers.end()) {
                auto candidates = std::count_if(entry.subscribers.begin(), entry.subscribers.end(),
                    [generation](const auto &subscriber) { return subscriber.generation == generation; });
                if (entry.subscribers.size() >= 64 || candidates >= 2) {
                    return;
                }
                entry.subscribers.push_back({});
                found = entry.subscribers.end() - 1;
                found->generation = generation;
                found->peer = source;
                found->token = nonce;
                // Each candidate receives a fresh server sequence. Its ACK commits
                // the endpoint for this member generation.
                ++sequence;
            }
            found->seen = now;
            if (found->sent == Clock::time_point{} || now - found->sent >= RETRY) {
                SendStatus(entry, *found, now);
            }
        } else if (kind == ACK && found != entry.subscribers.end() && SameEndpoint(source, found->peer) &&
                   nonce == found->token && revision == sequence) {
            found->acknowledged = revision;
            found->seen = now;
            entry.subscribers.erase(std::remove_if(entry.subscribers.begin(), entry.subscribers.end(),
                [&](const auto &subscriber) {
                    return subscriber.generation == generation &&
                           (subscriber.token != nonce || !SameEndpoint(source, subscriber.peer));
                }), entry.subscribers.end());
        }
    }

    void HandleTerminal(Socket &entry, const sockaddr_storage &source, uint8_t kind, uint8_t mask, const Token &nonce,
                        uint64_t revision, Clock::time_point now, const Frame &frame)
    {
        if (kind == STATUS && SameEndpoint(source, entry.peer) && nonce == token && revision >= receivedSequence &&
            (revision != receivedSequence || mask == state.available)) {
            receivedSequence = revision;
            state.changed |= state.known ? state.available ^ mask : 0;
            state.known = true;
            state.available = mask;
            in_addr gateway{};
            if (!link.localIpv4.empty() && inet_pton(AF_INET, link.gatewayIpv4.c_str(), &gateway) == 1) {
                state.reconfigureIpv4 = memcmp(&gateway, frame.data() + 32, sizeof(gateway)) != 0 &&
                                        std::any_of(frame.begin() + 32, frame.end(), [](uint8_t b) { return b != 0; });
            }
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
                HandleTerminal(entry, source, kind, mask, nonce, revision, now, frame);
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
    impl_->link.members = link.members;
    impl_->configurationChanged = impl_->configurationChanged || (gateway && impl_->link.localIpv4 != link.localIpv4);
    impl_->link.localIpv4 = link.localIpv4;
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
    bool changed = impl_->configurationChanged || impl_->state.available != (available & 3);
    impl_->configurationChanged = false;
    if (changed) {
        impl_->state.available = available & 3;
        ++impl_->sequence;
        NETMGR_EXT_LOG_I("[NearlinkIpShare][SharedSignal] members=%{public}zu mask=%{public}u sequence=%{public}llu",
                         link.members.size(), impl_->state.available, static_cast<unsigned long long>(impl_->sequence));
    }
    for (auto &entry : impl_->sockets) {
        entry.subscribers.erase(std::remove_if(entry.subscribers.begin(), entry.subscribers.end(), [&](const auto &s) {
            return now - s.seen > EXPIRY || impl_->MemberGeneration(s.peer) != s.generation;
        }), entry.subscribers.end());
        impl_->Receive(entry, now);
        for (auto &subscriber : entry.subscribers) {
            if (changed || (subscriber.acknowledged != impl_->sequence && now - subscriber.sent >= RETRY)) {
                impl_->SendStatus(entry, subscriber, now);
            }
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
    impl_->state.reconfigureIpv4 = false;
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
