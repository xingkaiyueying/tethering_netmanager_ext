/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */
#ifndef NETMANAGER_EXT_NEARLINK_IPSHARE_CONVERTER_H
#define NETMANAGER_EXT_NEARLINK_IPSHARE_CONVERTER_H

#include <napi/native_api.h>

#include "nearlink_ipshare_status.h"

namespace OHOS::NetManagerStandard {
class NearlinkIpShareConverter final {
public:
    static napi_value ToJs(napi_env env, const NearlinkIpShareStatus &status);
};
} // namespace OHOS::NetManagerStandard
#endif // NETMANAGER_EXT_NEARLINK_IPSHARE_CONVERTER_H
