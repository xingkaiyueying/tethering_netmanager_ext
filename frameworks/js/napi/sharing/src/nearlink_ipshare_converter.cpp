/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */
#include "nearlink_ipshare_converter.h"

#include "napi_utils.h"

namespace OHOS::NetManagerStandard {
namespace {
const char *RoleName(NearlinkIpShareRole role)
{
    switch (role) {
        case NearlinkIpShareRole::GATEWAY: return "GATEWAY";
        case NearlinkIpShareRole::TERMINAL: return "TERMINAL";
        default: return "NONE";
    }
}

const char *StateName(NearlinkIpShareState state)
{
    switch (state) {
        case NearlinkIpShareState::STARTING: return "STARTING";
        case NearlinkIpShareState::DISCOVERING: return "DISCOVERING";
        case NearlinkIpShareState::CONFIGURING: return "CONFIGURING";
        case NearlinkIpShareState::IFACE_READY: return "IFACE_READY";
        case NearlinkIpShareState::CHANNEL_READY: return "CHANNEL_READY";
        case NearlinkIpShareState::DHCP: return "DHCP";
        case NearlinkIpShareState::SERVING: return "SERVING";
        case NearlinkIpShareState::SERVING_NO_UPSTREAM: return "SERVING_NO_UPSTREAM";
        case NearlinkIpShareState::ACTIVE: return "ACTIVE";
        case NearlinkIpShareState::STOPPING: return "STOPPING";
        case NearlinkIpShareState::ERROR: return "ERROR";
        default: return "IDLE";
    }
}
} // namespace

napi_value NearlinkIpShareConverter::ToJs(napi_env env, const NearlinkIpShareStatus &status)
{
    napi_value value = NapiUtils::CreateObject(env);
    NapiUtils::SetStringPropertyUtf8(env, value, "role", RoleName(status.role));
    NapiUtils::SetStringPropertyUtf8(env, value, "state", StateName(status.state));
    NapiUtils::SetStringPropertyUtf8(env, value, "peerAddress", status.peerAddress);
    NapiUtils::SetStringPropertyUtf8(env, value, "ifaceName", status.ifaceName);
    NapiUtils::SetStringPropertyUtf8(env, value, "ipv4Address", status.ipv4Address);
    NapiUtils::SetBooleanProperty(env, value, "hasUpstream", status.hasUpstream);
    NapiUtils::SetStringPropertyUtf8(env, value, "errorStage", status.errorStage);
    NapiUtils::SetInt32Property(env, value, "errorCode", status.errorCode);
    return value;
}
} // namespace OHOS::NetManagerStandard
