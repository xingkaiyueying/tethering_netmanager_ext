/*
 * Copyright (C) 2026 Huawei Device Co., Ltd.
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
#ifndef NEARLINK_PEER_ADDRESS_POOL_H
#define NEARLINK_PEER_ADDRESS_POOL_H
#include <arpa/inet.h>
#include <cstdint>
#include <string>

namespace OHOS::NetManagerStandard {
// Product CIDR pools; one /24 and one /64 per shared gateway session. No public DHCP layout changes.
struct NearlinkPeerAddresses {
    std::string gateway, start, end, subnet, prefix;
    static bool ParsePool(const std::string &cidr, int family, uint8_t *binary, unsigned &bits)
    {
        auto slash = cidr.find('/');
        if (slash == std::string::npos || slash + 1 == cidr.size()) {
            return false;
        }
        bits = 0;
        for (size_t i = slash + 1; i < cidr.size(); ++i) {
            if (cidr[i] < '0' || cidr[i] > '9' || bits > 128) {
                return false;
            }
            bits = bits * 10 + cidr[i] - '0';
        }
        if (bits > (family == AF_INET ? 32u : 128u) || inet_pton(family, cidr.substr(0, slash).c_str(), binary) != 1) {
            return false;
        }
        for (unsigned i = bits; i < (family == AF_INET ? 32u : 128u); ++i) {
            if (binary[i / 8] & (0x80 >> (i % 8))) {
                return false;
            }
        }
        return true;
    }
    static bool Allocate(const std::string &v4, const std::string &v6, unsigned capacity,
                         NearlinkPeerAddresses &out, bool dual)
    {
        uint8_t ipv4[4]{}, ipv6[16]{};
        unsigned bits = 0;
        if (!capacity || capacity > 32 || !ParsePool(v4, AF_INET, ipv4, bits) || bits < 8 ||
            bits > 24) {
            return false;
        }
        // RFC1918 only; reject public, loopback, multicast and link-local pools.
        if (!(ipv4[0] == 10 || (ipv4[0] == 172 && ipv4[1] >= 16 && ipv4[1] <= 31) ||
              (ipv4[0] == 192 && ipv4[1] == 168))) {
            return false;
        }
        uint32_t base = (static_cast<uint32_t>(ipv4[0]) << 24) | (static_cast<uint32_t>(ipv4[1]) << 16) | (static_cast<uint32_t>(ipv4[2]) << 8) | ipv4[3];
        // Every seat joins the same subnet. The controller selects the session subnet.
        auto text = [](uint32_t n) {
            return std::to_string(n >> 24) + "." + std::to_string((n >> 16) & 255) + "." +
                   std::to_string((n >> 8) & 255) + "." + std::to_string(n & 255);
        };
        out.gateway = text(base + 1);
        out.start = text(base + 2);
        out.end = text(base + 254);
        out.subnet = text(base) + "/24";
        out.prefix.clear();
        if (!dual) {
            return true;
        }
        if (!ParsePool(v6, AF_INET6, ipv6, bits) || bits < 32 || bits > 64 ||
            !((ipv6[0] & 0xfe) == 0xfc || (ipv6[0] & 0xe0) == 0x20)) {
            return false;
        }
        char buffer[INET6_ADDRSTRLEN]{};
        if (!inet_ntop(AF_INET6, ipv6, buffer, sizeof(buffer))) {
            return false;
        }
        out.prefix = buffer;
        return true;
    }
    static bool Ipv6Conflicts(const std::string &prefix, const std::string &address, unsigned bits)
    {
        uint8_t a[16]{}, b[16]{};
        if (bits > 128 || inet_pton(AF_INET6, prefix.c_str(), a) != 1 || inet_pton(AF_INET6, address.c_str(), b) != 1) {
            return false;
        }
        unsigned common = bits < 64 ? bits : 64;
        for (unsigned i = 0; i < common; ++i) {
            if ((a[i / 8] ^ b[i / 8]) & (0x80 >> (i % 8))) {
                return false;
            }
        }
        return true;
    }
    bool Conflicts(const std::string &address, unsigned bits) const
    {
        in_addr a{}, g{};
        if (bits > 32 || inet_pton(AF_INET, address.c_str(), &a) != 1 || inet_pton(AF_INET, gateway.c_str(), &g) != 1) {
            return false;
        }
        unsigned common = bits < 24 ? bits : 24;
        uint32_t mask = common ? 0xffffffffu << (32 - common) : 0;
        return (ntohl(a.s_addr) & mask) == (ntohl(g.s_addr) & mask);
    }
};
} // namespace OHOS::NetManagerStandard
#endif
