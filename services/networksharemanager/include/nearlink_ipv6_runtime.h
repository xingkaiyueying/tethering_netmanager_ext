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
    bool Advertise(const NetLinkInfo *upstream, bool forwarding, bool dnsReady = true);
    bool Cleanup();
    bool HasPrefix() const
    {
        return !prefix_.empty();
    }
    const std::string &Gateway() const
    {
        return gateway_;
    }

private:
    bool Set(const std::string &key, const std::string &value);
    bool AddAddress(const std::string &address);
    bool SetToken();
    bool SelectPrefix(std::string &prefix, std::string &dns, bool configured,
                      std::chrono::steady_clock::time_point now);
    void PublishAdvertisement(const RaParams &params, const std::string &dns, bool changed);
    void ExpireRetiredPrefixes(std::chrono::steady_clock::time_point now);
    bool ReconcileGatewayAddress(std::chrono::steady_clock::time_point now);
    uint32_t ifindex_{0};
    short flags_{0};
    bool flagsOwned_{false}, tokenOwned_{false}, routeOwned_{false}, raStarted_{false}, prepared_{false},
        gatewayAddressOwned_{false};
    std::chrono::steady_clock::time_point advertisedAt_{};
    struct Retired {
        std::string prefix, gateway;
        std::chrono::steady_clock::time_point until;
        bool route;
    };
    std::vector<Retired> retired_;
    int lastRouterLifetime_{-1};
    std::map<std::string, std::string> settings_;
    std::vector<std::string> addresses_;
    std::string layer2_, prefix_, gateway_, dns_;
    std::shared_ptr<RouterAdvertisementDaemon> daemon_;
};
} // namespace OHOS::NetManagerStandard
#endif
