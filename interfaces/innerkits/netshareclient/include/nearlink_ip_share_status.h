/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */
#ifndef NETMANAGER_EXT_NEARLINK_IP_SHARE_STATUS_H
#define NETMANAGER_EXT_NEARLINK_IP_SHARE_STATUS_H

#include <cstdint>
#include <new>
#include <string>

#include "parcel.h"

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

    bool Marshalling(Parcel &parcel) const override
    {
        return parcel.WriteInt32(static_cast<int32_t>(role)) && parcel.WriteInt32(static_cast<int32_t>(state)) &&
            parcel.WriteString(peerAddress) && parcel.WriteString(ifaceName) && parcel.WriteString(ipv4Address) &&
            parcel.WriteBool(hasUpstream) && parcel.WriteString(errorStage) && parcel.WriteInt32(errorCode);
    }

    bool ReadFromParcel(Parcel &parcel)
    {
        role = static_cast<NearlinkIpShareRole>(parcel.ReadInt32());
        state = static_cast<NearlinkIpShareState>(parcel.ReadInt32());
        peerAddress = parcel.ReadString();
        ifaceName = parcel.ReadString();
        ipv4Address = parcel.ReadString();
        hasUpstream = parcel.ReadBool();
        errorStage = parcel.ReadString();
        errorCode = parcel.ReadInt32();
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
