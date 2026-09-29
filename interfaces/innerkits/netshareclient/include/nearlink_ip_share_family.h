/* Copyright (c) 2026 Huawei Device Co., Ltd. Licensed under the Apache License, Version 2.0. */
#ifndef NEARLINK_IP_SHARE_FAMILY_H
#define NEARLINK_IP_SHARE_FAMILY_H
#include <vector>
#include <set>
#include <arpa/inet.h>
#include "parcel.h"
namespace OHOS::NetManagerStandard {
struct NearlinkIpShareAddress {
    std::string address;
    uint32_t prefixLength{0}, scopeId{0}, origin{0}, dadState{0}, preferredLifetime{0}, validLifetime{0};
    bool Write(Parcel &p) const
    {
        return address.size() <= 45 && prefixLength <= 128 && preferredLifetime <= validLifetime &&
            p.WriteString(address) && p.WriteUint32(prefixLength) && p.WriteUint32(scopeId) &&
            p.WriteUint32(origin) && p.WriteUint32(dadState) && p.WriteUint32(preferredLifetime) &&
            p.WriteUint32(validLifetime);
    }
    bool Read(Parcel &p)
    {
        return p.ReadString(address) && address.size() <= 45 && p.ReadUint32(prefixLength) && prefixLength <= 128 &&
            p.ReadUint32(scopeId) && p.ReadUint32(origin) && p.ReadUint32(dadState) && dadState <= 4 &&
            p.ReadUint32(preferredLifetime) && p.ReadUint32(validLifetime) && preferredLifetime <= validLifetime;
    }
};
struct NearlinkIpShareRoute {
    std::string destination, gateway;
    uint32_t prefixLength{0}, scopeId{0}, lifetime{0};
    bool lifetimeKnown{false};
    bool Write(Parcel &p) const
    {
        return destination.size() <= 45 && gateway.size() <= 45 && prefixLength <= 128 &&
            p.WriteString(destination) && p.WriteString(gateway) && p.WriteUint32(prefixLength) &&
            p.WriteUint32(scopeId) && p.WriteUint32(lifetime) && p.WriteBool(lifetimeKnown);
    }
    bool Read(Parcel &p)
    {
        return p.ReadString(destination) && destination.size() <= 45 && p.ReadString(gateway) && gateway.size() <= 45 &&
            p.ReadUint32(prefixLength) && prefixLength <= 128 && p.ReadUint32(scopeId) && p.ReadUint32(lifetime) && p.ReadBool(lifetimeKnown);
    }
};
struct NearlinkIpShareDns {
    std::string address;
    uint32_t transportFamily{0}, source{0}, lifetime{0};
    bool lifetimeKnown{false};
    bool Write(Parcel &p) const
    {
        return address.size() <= 45 && p.WriteString(address) && p.WriteUint32(transportFamily) &&
            p.WriteUint32(source) && p.WriteUint32(lifetime) && p.WriteBool(lifetimeKnown);
    }
    bool Read(Parcel &p)
    {
        return p.ReadString(address) && address.size() <= 45 && p.ReadUint32(transportFamily) &&
            p.ReadUint32(source) && p.ReadUint32(lifetime) && p.ReadBool(lifetimeKnown);
    }
};
struct NearlinkIpShareError {
    uint32_t plane{0}, family{0};
    std::string stage;
    int32_t code{0};
    bool retryable{false};
    bool Write(Parcel &p) const
    {
        return stage.size() <= 31 && p.WriteUint32(plane) && p.WriteString(stage) && p.WriteUint32(family) &&
            p.WriteInt32(code) && p.WriteBool(retryable);
    }
    bool Read(Parcel &p)
    {
        return p.ReadUint32(plane) && plane <= 4 && p.ReadString(stage) && stage.size() <= 31 &&
            p.ReadUint32(family) && family <= 2 && p.ReadInt32(code) && p.ReadBool(retryable);
    }
};
struct NearlinkIpShareFamilyStatus {
    int32_t phase{0}; // DISABLED, CONFIGURING, AVAILABLE, FAILED
    bool configurationAvailable{false}, externalAvailable{false};
    int32_t validation{0}; // UNKNOWN, CHECKING, VALIDATED, FAILED
    std::vector<NearlinkIpShareAddress> addresses;
    std::vector<NearlinkIpShareRoute> routes;
    std::vector<NearlinkIpShareDns> dns;
    bool hasError{false};
    NearlinkIpShareError error;
    template<class T> static bool WriteList(Parcel &p, const std::vector<T> &items, size_t max)
    {
        if (items.size() > max || !p.WriteUint32(items.size())) return false;
        for (const auto &item : items) if (!item.Write(p)) return false;
        return true;
    }
    template<class T> static bool ReadList(Parcel &p, std::vector<T> &items, size_t max)
    {
        uint32_t count;
        if (!p.ReadUint32(count) || count > max) return false;
        items.resize(count);
        for (auto &item : items) if (!item.Read(p)) return false;
        return true;
    }
    bool Valid(bool ipv6) const
    {
        if (addresses.size() > (ipv6 ? 8u : 1u) || routes.size() > 16 || dns.size() > 8 ||
            (externalAvailable && (!configurationAvailable || validation != 2))) return false;
        const int af = ipv6 ? AF_INET6 : AF_INET;
        std::set<std::string> seen;
        for (const auto &a : addresses) {
            in6_addr binary{};
            if (a.prefixLength > (ipv6 ? 128u : 32u) || inet_pton(af, a.address.c_str(), &binary) != 1 ||
                !seen.insert(std::string(reinterpret_cast<const char *>(&binary), ipv6 ? 16 : 4)).second ||
                a.dadState > 4 || a.preferredLifetime > a.validLifetime) return false;
        }
        for (const auto &r : routes) {
            in6_addr binary{};
            if (r.prefixLength > (ipv6 ? 128u : 32u) || inet_pton(af, r.destination.c_str(), &binary) != 1 ||
                inet_pton(af, r.gateway.c_str(), &binary) != 1 ||
                (ipv6 && IN6_IS_ADDR_LINKLOCAL(&binary) && r.scopeId == 0)) return false;
        }
        for (const auto &d : dns) {
            in6_addr binary{};
            if ((d.transportFamily != 1 && d.transportFamily != 2) || d.source < 1 || d.source > 3 ||
                inet_pton(d.transportFamily == 1 ? AF_INET : AF_INET6, d.address.c_str(), &binary) != 1) return false;
        }
        return true;
    }
    bool Write(Parcel &p) const
    {
        return phase >= 0 && phase <= 3 && validation >= 0 && validation <= 3 &&
            p.WriteInt32(phase) && p.WriteBool(configurationAvailable) && p.WriteBool(externalAvailable) &&
            p.WriteInt32(validation) && WriteList(p, addresses, 8) && WriteList(p, routes, 16) &&
            WriteList(p, dns, 8) && p.WriteBool(hasError) && error.Write(p);
    }
    bool Read(Parcel &p)
    {
        return p.ReadInt32(phase) && phase >= 0 && phase <= 3 && p.ReadBool(configurationAvailable) &&
            p.ReadBool(externalAvailable) && p.ReadInt32(validation) && validation >= 0 && validation <= 3 &&
            ReadList(p, addresses, 8) && ReadList(p, routes, 16) && ReadList(p, dns, 8) &&
            p.ReadBool(hasError) && error.Read(p);
    }
};
} // namespace OHOS::NetManagerStandard
#endif
