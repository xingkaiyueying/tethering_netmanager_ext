/*
 * Copyright (c) 2024 Huawei Device Co., Ltd.
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

#ifndef ROUTER_ADVERTISEMENT_PARAMS_H
#define ROUTER_ADVERTISEMENT_PARAMS_H

#include <any>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace OHOS {
namespace NetManagerStandard {
static constexpr int32_t IPV6_MIN_MTU = 1400;
static constexpr uint8_t DEFAULT_HOPLIMIT = 254;

struct IpPrefix {
    in6_addr address = {};
    in6_addr prefix = {};
    uint32_t prefixesLength = 0;
    uint32_t validLifetime = 3600;
    uint32_t preferredLifetime = 3600;
};

class RaParams {
public:
    bool hasDefaultRoute_ = false;
    uint8_t hopLimit_ = DEFAULT_HOPLIMIT;
    int32_t mtu_ = IPV6_MIN_MTU;
    std::string name_;
    uint32_t index_ = 0;
    std::string macAddr_;
    std::vector<IpPrefix> prefixes_;
    std::vector<in6_addr> dnses_;
    // Explicit TUN parameters; legacy tethering retains its existing defaults.
    bool layer3_ = false;
    uint16_t routerLifetime_ = 3600;
    uint32_t rdnssLifetime_ = 3600;

public:
    RaParams();
    ~RaParams() = default;
    void Set(const RaParams &raParam);
    bool ContainsPrefix(const IpPrefix &pre);
    bool ContainsDns(const in6_addr &dns);
    void Clear();
};

} // namespace NetManagerStandard
} // namespace OHOS
#endif // #define ROUTER_ADVERTISEMENT_PARAMS_H
