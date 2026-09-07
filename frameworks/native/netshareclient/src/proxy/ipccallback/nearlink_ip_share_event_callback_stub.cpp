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

#include "nearlink_ip_share_event_callback_stub.h"

#include <memory>

#include "net_manager_constants.h"

namespace OHOS {
namespace NetManagerStandard {
int32_t NearlinkIpShareEventCallbackStub::OnRemoteRequest(uint32_t code, MessageParcel &data, MessageParcel &reply,
                                                          MessageOption &option)
{
    if (data.ReadInterfaceToken() != INearlinkIpShareEventCallback::GetDescriptor()) {
        return NETMANAGER_EXT_ERR_DESCRIPTOR_MISMATCH;
    }
    if (code != static_cast<uint32_t>(
        INearlinkIpShareEventCallback::Message::NEARLINK_IP_SHARE_STATE_CHANGED)) {
        return IPCObjectStub::OnRemoteRequest(code, data, reply, option);
    }
    std::unique_ptr<NearlinkIpShareStatus> status(data.ReadParcelable<NearlinkIpShareStatus>());
    if (status == nullptr) {
        return IPC_PROXY_ERR;
    }
    OnNearlinkIpShareStateChanged(*status);
    return NETMANAGER_EXT_SUCCESS;
}
} // namespace NetManagerStandard
} // namespace OHOS
