/* Copyright (c) 2026 Huawei Device Co., Ltd. Licensed under the Apache License, Version 2.0. */
#ifndef NEARLINK_IPV6_RUNTIME_H
#define NEARLINK_IPV6_RUNTIME_H
#include <map>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include "router_advertisement_daemon.h"
#include "net_link_info.h"
namespace OHOS::NetManagerStandard {
// Owns only settings on the current sleip0 ifindex. Never executes shell commands.
class NearlinkIpv6Runtime {
public:
    bool Prepare(bool gateway, const std::string &layer2);
    bool Advertise(const NetLinkInfo *upstream, bool forwarding);
    bool Cleanup();
    bool HasPrefix() const { return !prefix_.empty(); }
    const std::string &Gateway() const { return gateway_; }
private:
    bool Set(const std::string &key, const std::string &value);
    bool AddAddress(const std::string &address);
    bool SetToken();
    uint32_t ifindex_{0};
    short flags_{0};
    bool flagsOwned_{false}, tokenOwned_{false}, routeOwned_{false}, raStarted_{false}, prepared_{false}, gatewayAddressOwned_{false};
    std::chrono::steady_clock::time_point advertisedAt_{};
    struct Retired { std::string prefix, gateway; std::chrono::steady_clock::time_point until; bool route; };
    std::vector<Retired> retired_;
    int lastRouterLifetime_{-1};
    std::map<std::string, std::string> settings_;
    std::vector<std::string> addresses_;
    std::string layer2_, prefix_, gateway_, dns_;
    std::shared_ptr<RouterAdvertisementDaemon> daemon_;
};
} // namespace OHOS::NetManagerStandard
#endif
