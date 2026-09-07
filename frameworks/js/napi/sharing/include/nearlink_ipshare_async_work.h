/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */
#ifndef NETMANAGER_EXT_NEARLINK_IPSHARE_ASYNC_WORK_H
#define NETMANAGER_EXT_NEARLINK_IPSHARE_ASYNC_WORK_H

#include <napi/native_api.h>

namespace OHOS::NetManagerStandard {
class NearlinkIpShareContext;

class NearlinkIpShareAsyncWork final {
public:
    static void ExecIsSupported(napi_env env, void *data);
    static void ExecStartGateway(napi_env env, void *data);
    static void ExecStopGateway(napi_env env, void *data);
    static void ExecStartTerminal(napi_env env, void *data);
    static void ExecStopTerminal(napi_env env, void *data);
    static void ExecGetStatus(napi_env env, void *data);
    static void VoidCallback(napi_env env, napi_status status, void *data);
    static void SupportedCallback(napi_env env, napi_status status, void *data);
    static void StatusCallback(napi_env env, napi_status status, void *data);

private:
    enum class Operation { SUPPORT, START_GATEWAY, STOP_GATEWAY, START_TERMINAL, STOP_TERMINAL, GET_STATUS };
    static bool Execute(NearlinkIpShareContext *context, Operation operation);
    static bool DoSupport(NearlinkIpShareContext *context);
    static bool DoStartGateway(NearlinkIpShareContext *context);
    static bool DoStopGateway(NearlinkIpShareContext *context);
    static bool DoStartTerminal(NearlinkIpShareContext *context);
    static bool DoStopTerminal(NearlinkIpShareContext *context);
    static bool DoGetStatus(NearlinkIpShareContext *context);
    static napi_value MakeVoid(NearlinkIpShareContext *context);
    static napi_value MakeSupported(NearlinkIpShareContext *context);
    static napi_value MakeStatus(NearlinkIpShareContext *context);
};
} // namespace OHOS::NetManagerStandard
#endif // NETMANAGER_EXT_NEARLINK_IPSHARE_ASYNC_WORK_H
