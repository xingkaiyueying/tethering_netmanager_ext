/* Copyright (c) 2026 Huawei Device Co., Ltd. Licensed under the Apache License, Version 2.0. */
#ifndef NEARLINK_UPSTREAM_SIGNAL_H
#define NEARLINK_UPSTREAM_SIGNAL_H

#include <cstdint>
#include <memory>
#include <string>

namespace OHOS::NetManagerStandard {
// Private Demo UDP traffic over an existing IPoSL link; not an IPoSL/SSAP extension.
// The sharing worker owns all calls, including destruction. No receiver thread is created.
class NearlinkUpstreamSignal {
public:
    static constexpr uint8_t IPV4 = 1;
    static constexpr uint8_t IPV6 = 2;
    struct Link {
        std::string iface, localIpv4, gatewayIpv4;
        uint32_t ifindex{0};
        int32_t netId{-1};
        bool ipv6{false};
    };
    struct State {
        bool known{false};
        uint8_t available{0};
    };
    NearlinkUpstreamSignal();
    ~NearlinkUpstreamSignal();
    NearlinkUpstreamSignal(const NearlinkUpstreamSignal &) = delete;
    NearlinkUpstreamSignal &operator=(const NearlinkUpstreamSignal &) = delete;
    void Reset();
    void Gateway(const Link &link, uint8_t available);
    State Terminal(const Link &link);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool Prepare(const Link &link, bool gateway);
};
} // namespace OHOS::NetManagerStandard
#endif
