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

#ifndef I_NEARLINK_IP_SHARE_EVENT_CALLBACK_H
#define I_NEARLINK_IP_SHARE_EVENT_CALLBACK_H

#include <cstdint>

#include "iremote_broker.h"
#include "nearlink_ip_share_status.h"

namespace OHOS {
namespace NetManagerStandard {
class INearlinkIpShareEventCallback : public IRemoteBroker {
public:
    virtual void OnNearlinkIpShareStateChanged(const NearlinkIpShareStatus &status) = 0;

    enum class Message : uint32_t {
        NEARLINK_IP_SHARE_STATE_CHANGED = 0,
    };

    DECLARE_INTERFACE_DESCRIPTOR(u"OHOS.NetManagerStandard.INearlinkIpShareEventCallback");
};
} // namespace NetManagerStandard
} // namespace OHOS
#endif // I_NEARLINK_IP_SHARE_EVENT_CALLBACK_H
