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
    if ((paramsCount != PARAM_JUST_OPTIONS && paramsCount != PARAM_OPTIONS_AND_CALLBACK) ||
        NapiUtils::GetValueType(GetEnv(), params[0]) != napi_string) {
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
    if (paramsCount == PARAM_OPTIONS_AND_CALLBACK) {
        if (NapiUtils::GetValueType(GetEnv(), params[1]) == napi_undefined) {
            SetParseOK(true);
            return;
        }
        if (NapiUtils::GetValueType(GetEnv(), params[1]) != napi_object) {
            SetErrorCode(NETMANAGER_EXT_ERR_PARAMETER_ERROR);
            SetNeedThrowException(true);
            return;
        }
        napi_value modeValue = nullptr;
        if (napi_get_named_property(GetEnv(), params[1], "mode", &modeValue) != napi_ok ||
            NapiUtils::GetValueType(GetEnv(), modeValue) != napi_string) {
            SetErrorCode(NETMANAGER_EXT_ERR_PARAMETER_ERROR);
            SetNeedThrowException(true);
            return;
        }
        const auto mode = NapiUtils::GetStringFromValueUtf8(GetEnv(), modeValue);
        if (mode != "IPV4" && mode != "DUAL_STACK") {
            SetErrorCode(NETMANAGER_EXT_ERR_PARAMETER_ERROR);
            SetNeedThrowException(true);
            return;
        }
        mode_ = mode == "IPV4" ? 1 : 3;
        hasMode_ = true;
    }
    SetParseOK(true);
}

int32_t NearlinkIpShareContext::GetMode() const { return mode_; }
bool NearlinkIpShareContext::HasMode() const { return hasMode_; }
void NearlinkIpShareContext::SetCapabilities(const NearlinkIpShareCapabilities &value) { capabilities_ = value; }
const NearlinkIpShareCapabilities &NearlinkIpShareContext::GetCapabilities() const { return capabilities_; }

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
