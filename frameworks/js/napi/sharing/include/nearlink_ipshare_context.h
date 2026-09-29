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
#ifndef NETMANAGER_EXT_NEARLINK_IPSHARE_CONTEXT_H
#define NETMANAGER_EXT_NEARLINK_IPSHARE_CONTEXT_H

#include "base_context.h"
#include "nearlink_ip_share_status.h"

namespace OHOS::NetManagerStandard {
class NearlinkIpShareContext final : public BaseContext {
public:
    NearlinkIpShareContext() = delete;
    explicit NearlinkIpShareContext(napi_env env, std::shared_ptr<EventManager> &manager);
    void ParseParams(napi_value *params, size_t paramsCount);

    const std::string &GetPeerAddress() const;
    int32_t GetMode() const;
    bool HasMode() const;
    void SetCapabilities(const NearlinkIpShareCapabilities &capabilities);
    const NearlinkIpShareCapabilities &GetCapabilities() const;
    void SetSupported(bool supported);
    bool IsSupported() const;
    void SetStatus(const NearlinkIpShareStatus &status);
    const NearlinkIpShareStatus &GetStatus() const;

private:
    std::string peerAddress_;
    int32_t mode_ {1};
    bool hasMode_ {false};
    NearlinkIpShareCapabilities capabilities_;
    bool supported_ {false};
    NearlinkIpShareStatus status_;
};
} // namespace OHOS::NetManagerStandard
#endif // NETMANAGER_EXT_NEARLINK_IPSHARE_CONTEXT_H
