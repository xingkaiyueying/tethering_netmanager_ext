/* Copyright (c) 2026 Huawei Device Co., Ltd. Licensed under the Apache License, Version 2.0. */
#ifndef NEARLINK_PEER_ADDRESS_POOL_H
#define NEARLINK_PEER_ADDRESS_POOL_H
#include <arpa/inet.h>
#include <cstdint>
#include <string>

namespace OHOS::NetManagerStandard {
// Product CIDR pools; one /24 and one /64 per seat. No public DHCP layout changes.
struct NearlinkPeerAddresses {
    std::string gateway, start, end, subnet, prefix;
    static bool ParsePool(const std::string &cidr, int family, uint8_t *binary, unsigned &bits)
    {
        auto slash = cidr.find('/');
        if (slash == std::string::npos || slash + 1 == cidr.size())
            return false;
        bits = 0;
        for (size_t i = slash + 1; i < cidr.size(); ++i) {
            if (cidr[i] < '0' || cidr[i] > '9' || bits > 128)
                return false;
            bits = bits * 10 + cidr[i] - '0';
        }
        if (bits > (family == AF_INET ? 32u : 128u) || inet_pton(family, cidr.substr(0, slash).c_str(), binary) != 1)
            return false;
        for (unsigned i = bits; i < (family == AF_INET ? 32u : 128u); ++i) {
            if (binary[i / 8] & (0x80 >> (i % 8)))
                return false;
        }
        return true;
    }
    static bool Allocate(const std::string &v4, const std::string &v6, unsigned capacity, unsigned slot,
                         NearlinkPeerAddresses &out, bool dual)
    {
        uint8_t ipv4[4]{}, ipv6[16]{};
        unsigned bits = 0;
        if (!capacity || capacity > 32 || slot >= capacity || !ParsePool(v4, AF_INET, ipv4, bits) || bits < 8 ||
            bits > 24 || capacity > (1u << (24 - bits)))
            return false;
        // RFC1918 only; reject public, loopback, multicast and link-local pools.
        if (!(ipv4[0] == 10 || (ipv4[0] == 172 && ipv4[1] >= 16 && ipv4[1] <= 31) ||
              (ipv4[0] == 192 && ipv4[1] == 168)))
            return false;
        uint32_t base = (uint32_t(ipv4[0]) << 24) | (uint32_t(ipv4[1]) << 16) | (uint32_t(ipv4[2]) << 8) | ipv4[3];
        base += slot * 256;
        auto text = [](uint32_t n) {
            return std::to_string(n >> 24) + "." + std::to_string((n >> 16) & 255) + "." +
                   std::to_string((n >> 8) & 255) + "." + std::to_string(n & 255);
        };
        out.gateway = text(base + 1);
        out.start = text(base + 2);
        out.end = text(base + 20);
        out.subnet = text(base) + "/24";
        out.prefix.clear();
        if (!dual)
            return true;
        if (!ParsePool(v6, AF_INET6, ipv6, bits) || bits < 32 || bits > 64 ||
            (bits > 59 && capacity > (1u << (64 - bits))) || !((ipv6[0] & 0xfe) == 0xfc || (ipv6[0] & 0xe0) == 0x20))
            return false;
        // Canonical pool has zero host bits, so slot occupies the low subnet bits.
        for (unsigned i = 0; i < 4; ++i)
            ipv6[7 - i] |= (slot >> (8 * i)) & 255;
        char buffer[INET6_ADDRSTRLEN]{};
        if (!inet_ntop(AF_INET6, ipv6, buffer, sizeof(buffer)))
            return false;
        out.prefix = buffer;
        return true;
    }
    bool Conflicts(const std::string &address, unsigned bits) const
    {
        in_addr a{}, g{};
        if (bits > 32 || inet_pton(AF_INET, address.c_str(), &a) != 1 || inet_pton(AF_INET, gateway.c_str(), &g) != 1)
            return false;
        unsigned common = bits < 24 ? bits : 24;
        uint32_t mask = common ? 0xffffffffu << (32 - common) : 0;
        return (ntohl(a.s_addr) & mask) == (ntohl(g.s_addr) & mask);
    }
};
} // namespace OHOS::NetManagerStandard
#endif
