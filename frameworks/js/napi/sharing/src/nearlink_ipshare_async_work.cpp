/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
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
        case Operation::START_GATEWAY:
            if (!context->GetPeerAddress().empty()) {
                ret = client->StartNearlinkGateway(context->GetPeerAddress());
            }
            break;
        case Operation::STOP_GATEWAY:
            ret = client->StopNearlinkGateway();
            break;
        case Operation::START_TERMINAL:
            if (!context->GetPeerAddress().empty()) {
                ret = client->StartNearlinkTerminal(context->GetPeerAddress());
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

void NearlinkIpShareAsyncWork::StatusCallback(napi_env env, napi_status status, void *data)
{
    BaseAsyncWork::AsyncWorkCallback<NearlinkIpShareContext, NearlinkIpShareAsyncWork::MakeStatus>(env, status, data);
}
} // namespace OHOS::NetManagerStandard
