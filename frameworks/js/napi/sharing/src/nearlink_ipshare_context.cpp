/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */
#include "nearlink_ipshare_context.h"

#include <cctype>

#include "constant.h"
#include "napi_utils.h"
#include "net_manager_constants.h"

namespace OHOS::NetManagerStandard {
NearlinkIpShareContext::NearlinkIpShareContext(napi_env env, std::shared_ptr<EventManager> &manager)
    : BaseContext(env, manager)
{
}

void NearlinkIpShareContext::ParseParams(napi_value *params, size_t paramsCount)
{
    if (paramsCount == PARAM_NONE) {
        SetParseOK(true);
        return;
    }
    if (paramsCount != PARAM_JUST_OPTIONS || NapiUtils::GetValueType(GetEnv(), params[0]) != napi_string) {
        SetErrorCode(NETMANAGER_EXT_ERR_PARAMETER_ERROR);
        SetNeedThrowException(true);
        return;
    }
    peerAddress_ = NapiUtils::GetStringFromValueUtf8(GetEnv(), params[0]);
    bool valid = peerAddress_.size() == 17;
    for (size_t i = 0; valid && i < peerAddress_.size(); ++i) {
        valid = i % 3 == 2 ? peerAddress_[i] == ':' : std::isxdigit(static_cast<unsigned char>(peerAddress_[i])) != 0;
    }
    if (!valid) {
        SetErrorCode(NETMANAGER_EXT_ERR_PARAMETER_ERROR);
        SetNeedThrowException(true);
        return;
    }
    SetParseOK(true);
}

const std::string &NearlinkIpShareContext::GetPeerAddress() const
{
    return peerAddress_;
}

void NearlinkIpShareContext::SetSupported(bool supported)
{
    supported_ = supported;
}

bool NearlinkIpShareContext::IsSupported() const
{
    return supported_;
}

void NearlinkIpShareContext::SetStatus(const NearlinkIpShareStatus &status)
{
    status_ = status;
}

const NearlinkIpShareStatus &NearlinkIpShareContext::GetStatus() const
{
    return status_;
}
} // namespace OHOS::NetManagerStandard
