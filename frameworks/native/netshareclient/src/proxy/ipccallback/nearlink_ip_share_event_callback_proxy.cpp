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

#include "nearlink_ip_share_event_callback_proxy.h"

#include "ipc_types.h"
#include "netmgr_ext_log_wrapper.h"

namespace OHOS {
namespace NetManagerStandard {
NearlinkIpShareEventCallbackProxy::NearlinkIpShareEventCallbackProxy(const sptr<IRemoteObject> &object)
    : IRemoteProxy<INearlinkIpShareEventCallback>(object)
{
}

void NearlinkIpShareEventCallbackProxy::OnNearlinkIpShareStateChanged(const NearlinkIpShareStatus &status)
{
    auto remote = Remote();
    if (remote == nullptr) {
        NETMGR_EXT_LOG_E("[NearlinkIpShare][Callback] remote is null");
        return;
    }
    MessageParcel data;
    MessageParcel reply;
    MessageOption option;
    if (!data.WriteInterfaceToken(INearlinkIpShareEventCallback::GetDescriptor()) || !data.WriteParcelable(&status)) {
        NETMGR_EXT_LOG_E("[NearlinkIpShare][Callback] status parcel write failed");
        return;
    }
    int32_t ret = remote->SendRequest(
        static_cast<uint32_t>(INearlinkIpShareEventCallback::Message::NEARLINK_IP_SHARE_STATE_CHANGED),
        data, reply, option);
    if (ret != ERR_NONE) {
        NETMGR_EXT_LOG_E("[NearlinkIpShare][Callback] send failed code=%{public}d", ret);
    }
}
} // namespace NetManagerStandard
} // namespace OHOS
