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
#ifndef NETMANAGER_EXT_NEARLINK_IPSHARE_CONVERTER_H
#define NETMANAGER_EXT_NEARLINK_IPSHARE_CONVERTER_H

#include <napi/native_api.h>

#include "nearlink_ip_share_status.h"

namespace OHOS::NetManagerStandard {
class NearlinkIpShareConverter final {
public:
    static napi_value ToJs(napi_env env, const NearlinkIpShareStatus &status);
};
} // namespace OHOS::NetManagerStandard
#endif // NETMANAGER_EXT_NEARLINK_IPSHARE_CONVERTER_H
