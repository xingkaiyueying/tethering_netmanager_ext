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
#ifndef NETMANAGER_EXT_NEARLINK_IP_SHARE_STATUS_H
#define NETMANAGER_EXT_NEARLINK_IP_SHARE_STATUS_H

#include <cstdint>
#include <new>
#include <string>

#include "parcel.h"
#include "nearlink_ip_share_family.h"

namespace OHOS::NetManagerStandard {
enum class NearlinkIpShareRole : int32_t {
    NONE = 0,
    GATEWAY = 1,
    TERMINAL = 2,
};

enum class NearlinkIpShareState : int32_t {
    IDLE = 0,
    STARTING = 1,
    DISCOVERING = 2,
    CONFIGURING = 3,
    IFACE_READY = 4,
    CHANNEL_READY = 5,
    DHCP = 6,
    SERVING = 7,
    SERVING_NO_UPSTREAM = 8,
    ACTIVE = 9,
    STOPPING = 10,
    ERROR = 11,
};

class NearlinkIpShareCapabilities final : public Parcelable {
public:
    bool identifierPresent{false}, peerCapabilityKnown{false};
    int32_t discoveryState{0};
    std::vector<int32_t> localModes{1, 3}, peerModes;
    bool Marshalling(Parcel &p) const override
    {
        if (!p.WriteBool(identifierPresent) || !p.WriteInt32(discoveryState)) return false;
        for (const auto *modes : {&localModes, &peerModes}) {
            if (modes->size() > 2 || !p.WriteUint32(modes->size())) return false;
            for (auto mode : *modes) if ((mode != 1 && mode != 3) || !p.WriteInt32(mode)) return false;
        }
        return p.WriteBool(peerCapabilityKnown);
    }
    static NearlinkIpShareCapabilities *Unmarshalling(Parcel &p)
    {
        auto value = new (std::nothrow) NearlinkIpShareCapabilities;
        if (!value) return nullptr;
        bool ok = p.ReadBool(value->identifierPresent) && p.ReadInt32(value->discoveryState) &&
            value->discoveryState >= 0 && value->discoveryState <= 2;
        for (auto *modes : {&value->localModes, &value->peerModes}) {
            uint32_t size = 0;
            ok = ok && p.ReadUint32(size) && size <= 2;
            if (!ok) break;
            modes->clear();
            for (uint32_t i = 0; i < size; ++i) {
                int32_t mode;
                if (!p.ReadInt32(mode) || (mode != 1 && mode != 3)) { ok = false; break; }
                modes->push_back(mode);
            }
        }
        if (!ok || !p.ReadBool(value->peerCapabilityKnown)) { delete value; return nullptr; }
        return value;
    }
};

class NearlinkIpShareStatus final : public Parcelable {
public:
    NearlinkIpShareRole role {NearlinkIpShareRole::NONE};
    NearlinkIpShareState state {NearlinkIpShareState::IDLE};
    std::string peerAddress;
    std::string ifaceName;
    std::string ipv4Address;
    bool hasUpstream {false};
    std::string errorStage;
    int32_t errorCode {0};

    std::string contextId, fallbackReason;
    uint64_t generation{0}, sequence{0};
    int32_t requestedMode{1}, selectedMode{0}, netId{-1};
    bool serviceReady{false};
    NearlinkIpShareFamilyStatus ipv4, ipv6;

    bool Marshalling(Parcel &parcel) const override
    {
        return peerAddress.size() <= 17 && ifaceName.size() <= 15 && ipv4Address.size() <= 15 &&
            errorStage.size() <= 31 && contextId.size() <= 64 && fallbackReason.size() <= 128 &&
            ipv4.Valid(false) && ipv6.Valid(true) && parcel.WriteInt32(static_cast<int32_t>(role)) && parcel.WriteInt32(static_cast<int32_t>(state)) &&
            parcel.WriteString(peerAddress) && parcel.WriteString(ifaceName) && parcel.WriteString(ipv4Address) &&
            parcel.WriteBool(hasUpstream) && parcel.WriteString(errorStage) && parcel.WriteInt32(errorCode) &&
            parcel.WriteString(contextId) && parcel.WriteUint64(generation) && parcel.WriteUint64(sequence) &&
            parcel.WriteInt32(requestedMode) && parcel.WriteInt32(selectedMode) && parcel.WriteString(fallbackReason) &&
            parcel.WriteInt32(netId) && parcel.WriteBool(serviceReady) && ipv4.Write(parcel) && ipv6.Write(parcel);
    }

    bool ReadFromParcel(Parcel &parcel)
    {
        NearlinkIpShareStatus next;
        int32_t roleValue, stateValue;
        if (!parcel.ReadInt32(roleValue) || roleValue < 0 || roleValue > 2 ||
            !parcel.ReadInt32(stateValue) || stateValue < 0 || stateValue > 11 ||
            !parcel.ReadString(next.peerAddress) || next.peerAddress.size() > 17 ||
            !parcel.ReadString(next.ifaceName) || next.ifaceName.size() > 15 ||
            !parcel.ReadString(next.ipv4Address) || next.ipv4Address.size() > 15 ||
            !parcel.ReadBool(next.hasUpstream) || !parcel.ReadString(next.errorStage) || next.errorStage.size() > 31 ||
            !parcel.ReadInt32(next.errorCode) || !parcel.ReadString(next.contextId) || next.contextId.size() > 64 ||
            !parcel.ReadUint64(next.generation) || !parcel.ReadUint64(next.sequence) ||
            !parcel.ReadInt32(next.requestedMode) || (next.requestedMode != 1 && next.requestedMode != 3) ||
            !parcel.ReadInt32(next.selectedMode) ||
            (next.selectedMode != 0 && next.selectedMode != 1 && next.selectedMode != 3) ||
            !parcel.ReadString(next.fallbackReason) || next.fallbackReason.size() > 128 ||
            !parcel.ReadInt32(next.netId) || !parcel.ReadBool(next.serviceReady) ||
            !next.ipv4.Read(parcel) || !next.ipv6.Read(parcel) || !next.ipv4.Valid(false) || !next.ipv6.Valid(true)) return false;
        next.role = static_cast<NearlinkIpShareRole>(roleValue);
        next.state = static_cast<NearlinkIpShareState>(stateValue);
        *this = next;
        return true;
    }

    static NearlinkIpShareStatus *Unmarshalling(Parcel &parcel)
    {
        auto *status = new (std::nothrow) NearlinkIpShareStatus();
        if (status == nullptr || !status->ReadFromParcel(parcel)) {
            delete status;
            return nullptr;
        }
        return status;
    }
};
} // namespace OHOS::NetManagerStandard
#endif // NETMANAGER_EXT_NEARLINK_IP_SHARE_STATUS_H
