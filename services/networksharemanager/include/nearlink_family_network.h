/* Copyright (c) 2026 Huawei Device Co., Ltd. Licensed under the Apache License, Version 2.0. */
#ifndef NEARLINK_FAMILY_NETWORK_H
#define NEARLINK_FAMILY_NETWORK_H

#include <algorithm>
#include <chrono>
#include "net_link_info.h"

namespace OHOS::NetManagerStandard {
// Worker-owned desired state. NetConn applies the aggregate as a diff on one supplier/netId.
// Each family owns its DNS references; deduplication happens only in the published view.
class NearlinkFamilyNetwork {
public:
    using Clock = std::chrono::steady_clock;
    NetLinkInfo ipv4;
    NetLinkInfo ipv6;
    Clock::time_point leaseExpiry {};

    static bool Available(const NetLinkInfo &link)
    {
        return !link.netAddrList_.empty() && !link.routeList_.empty() && !link.dnsList_.empty();
    }

    NetLinkInfo Aggregate() const
    {
        NetLinkInfo link;
        link.ifaceName_ = "sleip0";
        link.mtu_ = 1500;
        link.isUserDefinedDnsServer_ = true;
        for (const auto *family : {&ipv4, &ipv6}) {
            link.netAddrList_.insert(link.netAddrList_.end(), family->netAddrList_.begin(), family->netAddrList_.end());
            link.routeList_.insert(link.routeList_.end(), family->routeList_.begin(), family->routeList_.end());
            for (const auto &dns : family->dnsList_) {
                if (std::none_of(link.dnsList_.begin(), link.dnsList_.end(), [&dns](const auto &existing) {
                    return existing.family_ == dns.family_ && existing.address_ == dns.address_;
                })) link.dnsList_.push_back(dns);
            }
        }
        return link;
    }

    bool ExpireLease(Clock::time_point now)
    {
        if (!ipv4.netAddrList_.empty() && now >= leaseExpiry) {
            ipv4 = NetLinkInfo{};
            return true;
        }
        return false;
    }
};
} // namespace OHOS::NetManagerStandard
#endif
