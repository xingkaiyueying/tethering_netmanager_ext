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

#ifndef NEARLINK_IP_SHARE_EVENT_CALLBACK_PROXY_H
#define NEARLINK_IP_SHARE_EVENT_CALLBACK_PROXY_H

#include "inearlink_ip_share_event_callback.h"
#include "iremote_proxy.h"

namespace OHOS {
namespace NetManagerStandard {
class NearlinkIpShareEventCallbackProxy : public IRemoteProxy<INearlinkIpShareEventCallback> {
public:
    explicit NearlinkIpShareEventCallbackProxy(const sptr<IRemoteObject> &object);
    ~NearlinkIpShareEventCallbackProxy() override = default;

    void OnNearlinkIpShareStateChanged(const NearlinkIpShareStatus &status) override;

private:
    static inline BrokerDelegator<NearlinkIpShareEventCallbackProxy> delegator_;
};
} // namespace NetManagerStandard
} // namespace OHOS
#endif // NEARLINK_IP_SHARE_EVENT_CALLBACK_PROXY_H
