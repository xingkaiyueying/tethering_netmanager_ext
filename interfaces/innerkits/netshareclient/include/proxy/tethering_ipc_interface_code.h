/*
* Copyright (c) 2023 Huawei Device Co., Ltd.
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

#ifndef TETHERING_IPC_INTERFACE_CODE_H
#define TETHERING_IPC_INTERFACE_CODE_H

/* SAID:1154 */
namespace OHOS {
namespace NetManagerStandard {
enum class TetheringInterfaceCode {
    CMD_GET_SHARING_SUPPORTED,
    CMD_GET_IS_SHARING,
    CMD_START_NETWORKSHARE,
    CMD_STOP_NETWORKSHARE,
    CMD_GET_SHARABLE_REGEXS,
    CMD_GET_SHARING_STATE,
    CMD_GET_SHARING_IFACES,
    CMD_REGISTER_EVENT_CALLBACK,
    CMD_UNREGISTER_EVENT_CALLBACK,
    CMD_GET_ACTIVATE_INTERFACE,
    CMD_GET_RX_BYTES,
    CMD_GET_TX_BYTES,
    CMD_GET_TOTAL_BYTES,
    CMD_SET_CONFIG,
};

enum class TetheringEventInterfaceCode {
    GLOBAL_SHARING_STATE_CHANGED,
    INTERFACE_SHARING_STATE_CHANGED,
    SHARING_UPSTREAM_CHANGED,
};
} // namespace NetManagerStandard
} // namespace OHOS
#endif // TETHERING_IPC_INTERFACE_CODE_H
