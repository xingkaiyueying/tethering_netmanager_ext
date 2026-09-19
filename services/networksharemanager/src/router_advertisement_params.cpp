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

#include "netmgr_ext_log_wrapper.h"
#include "router_advertisement_params.h"

namespace OHOS {
namespace NetManagerStandard {
RaParams::RaParams() : hasDefaultRoute_(false), hopLimit_(DEFAULT_HOPLIMIT), mtu_(IPV6_MIN_MTU) {}

void RaParams::Set(const RaParams &raParam)
{
    hasDefaultRoute_ = raParam.hasDefaultRoute_;
    hopLimit_ = raParam.hopLimit_;
    mtu_ = raParam.mtu_;
    prefixes_ = raParam.prefixes_;
    dnses_ = raParam.dnses_;
    macAddr_ = raParam.macAddr_;
    layer3_ = raParam.layer3_;
    routerLifetime_ = raParam.routerLifetime_;
    rdnssLifetime_ = raParam.rdnssLifetime_;
}

bool RaParams::ContainsPrefix(const IpPrefix &prefix)
{
    for (auto &ip : prefixes_) {
        if ((std::memcmp(ip.prefix.s6_addr, prefix.prefix.s6_addr, sizeof(in6_addr)) == 0) &&
            ip.prefixesLength == prefix.prefixesLength) {
            return true;
        }
    }
    return false;
}

bool RaParams::ContainsDns(const in6_addr &dns)
{
    for (auto &d : dnses_) {
        if (std::memcmp(d.s6_addr, dns.s6_addr, sizeof(in6_addr)) == 0) {
            return true;
        }
    }
    return false;
}

void RaParams::Clear()
{
    prefixes_.clear();
    dnses_.clear();
}

} // namespace NetManagerStandard
} // namespace OHOS
