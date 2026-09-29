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
#include "nearlink_ipshare_async_work.h"

#include "base_async_work.h"
#include "napi_utils.h"
#include "nearlink_ipshare_context.h"
#include "nearlink_ipshare_converter.h"
#include "net_manager_constants.h"
#include "networkshare_client.h"

namespace OHOS::NetManagerStandard {
bool NearlinkIpShareAsyncWork::Execute(NearlinkIpShareContext *context, Operation operation)
{
    if (((operation == Operation::SUPPORT || operation == Operation::CAPABILITIES) &&
        (context->HasMode() || context->GetPeerAddress().empty())) ||
        ((operation == Operation::STOP_GATEWAY || operation == Operation::STOP_TERMINAL ||
            operation == Operation::GET_STATUS) && !context->GetPeerAddress().empty())) {
        context->SetErrorCode(NETMANAGER_EXT_ERR_PARAMETER_ERROR);
        return false;
    }
    auto client = DelayedSingleton<NetworkShareClient>::GetInstance();
    int32_t ret = NETMANAGER_EXT_ERR_PARAMETER_ERROR;
    switch (operation) {
        case Operation::SUPPORT: {
            bool supported = false;
            if (!context->GetPeerAddress().empty()) {
                ret = client->IsNearlinkIpShareSupported(context->GetPeerAddress(), supported);
                context->SetSupported(supported);
            }
            break;
        }
        case Operation::CAPABILITIES: {
            NearlinkIpShareCapabilities capabilities;
            ret = client->QueryNearlinkIpShareCapabilities(context->GetPeerAddress(), capabilities);
            context->SetCapabilities(capabilities);
            break;
        }
        case Operation::START_GATEWAY:
            if (!context->GetPeerAddress().empty()) {
                ret = context->HasMode() ? client->StartNearlinkGatewayWithMode(context->GetPeerAddress(),
                    context->GetMode()) : client->StartNearlinkGateway(context->GetPeerAddress());
            }
            break;
        case Operation::STOP_GATEWAY:
            ret = client->StopNearlinkGateway();
            break;
        case Operation::START_TERMINAL:
            if (!context->GetPeerAddress().empty()) {
                ret = context->HasMode() ? client->StartNearlinkTerminalWithMode(context->GetPeerAddress(),
                    context->GetMode()) : client->StartNearlinkTerminal(context->GetPeerAddress());
            }
            break;
        case Operation::STOP_TERMINAL:
            ret = client->StopNearlinkTerminal();
            break;
        case Operation::GET_STATUS: {
            NearlinkIpShareStatus status;
            ret = client->GetNearlinkIpShareStatus(status);
            context->SetStatus(status);
            break;
        }
    }
    if (ret != NETMANAGER_EXT_SUCCESS) {
        context->SetErrorCode(ret);
        return false;
    }
    return true;
}

#define DEFINE_DO(name, operation) \
    bool NearlinkIpShareAsyncWork::name(NearlinkIpShareContext *context) \
    { \
        return Execute(context, Operation::operation); \
    }
DEFINE_DO(DoSupport, SUPPORT)
DEFINE_DO(DoCapabilities, CAPABILITIES)
DEFINE_DO(DoStartGateway, START_GATEWAY)
DEFINE_DO(DoStopGateway, STOP_GATEWAY)
DEFINE_DO(DoStartTerminal, START_TERMINAL)
DEFINE_DO(DoStopTerminal, STOP_TERMINAL)
DEFINE_DO(DoGetStatus, GET_STATUS)
#undef DEFINE_DO

#define DEFINE_EXEC(name, executor) \
    void NearlinkIpShareAsyncWork::name(napi_env env, void *data) \
    { \
        BaseAsyncWork::ExecAsyncWork<NearlinkIpShareContext, NearlinkIpShareAsyncWork::executor>(env, data); \
    }

DEFINE_EXEC(ExecIsSupported, DoSupport)
DEFINE_EXEC(ExecGetCapabilities, DoCapabilities)
DEFINE_EXEC(ExecStartGateway, DoStartGateway)
DEFINE_EXEC(ExecStopGateway, DoStopGateway)
DEFINE_EXEC(ExecStartTerminal, DoStartTerminal)
DEFINE_EXEC(ExecStopTerminal, DoStopTerminal)
DEFINE_EXEC(ExecGetStatus, DoGetStatus)
#undef DEFINE_EXEC

napi_value NearlinkIpShareAsyncWork::MakeVoid(NearlinkIpShareContext *context)
{
    return NapiUtils::GetUndefined(context->GetEnv());
}

napi_value NearlinkIpShareAsyncWork::MakeSupported(NearlinkIpShareContext *context)
{
    return NapiUtils::GetBoolean(context->GetEnv(), context->IsSupported());
}

napi_value NearlinkIpShareAsyncWork::MakeCapabilities(NearlinkIpShareContext *context)
{
    return NearlinkIpShareConverter::CapabilitiesToJs(context->GetEnv(), context->GetCapabilities());
}

napi_value NearlinkIpShareAsyncWork::MakeStatus(NearlinkIpShareContext *context)
{
    return NearlinkIpShareConverter::ToJs(context->GetEnv(), context->GetStatus());
}

void NearlinkIpShareAsyncWork::VoidCallback(napi_env env, napi_status status, void *data)
{
    BaseAsyncWork::AsyncWorkCallback<NearlinkIpShareContext, NearlinkIpShareAsyncWork::MakeVoid>(env, status, data);
}

void NearlinkIpShareAsyncWork::SupportedCallback(napi_env env, napi_status status, void *data)
{
    BaseAsyncWork::AsyncWorkCallback<NearlinkIpShareContext, NearlinkIpShareAsyncWork::MakeSupported>(env, status, data);
}

void NearlinkIpShareAsyncWork::CapabilitiesCallback(napi_env env, napi_status status, void *data)
{
    BaseAsyncWork::AsyncWorkCallback<NearlinkIpShareContext, NearlinkIpShareAsyncWork::MakeCapabilities>(env, status, data);
}

void NearlinkIpShareAsyncWork::StatusCallback(napi_env env, napi_status status, void *data)
{
    BaseAsyncWork::AsyncWorkCallback<NearlinkIpShareContext, NearlinkIpShareAsyncWork::MakeStatus>(env, status, data);
}
} // namespace OHOS::NetManagerStandard
