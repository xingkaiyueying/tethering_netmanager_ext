/*
 * Copyright (c) 2022-2023 Huawei Device Co., Ltd.
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

#include "networkshare_client.h"

#include <thread>

#include "iservice_registry.h"
#include "system_ability_definition.h"
#include "net_manager_constants.h"
#include "netmgr_ext_log_wrapper.h"
#include "network_share_service_proxy.h"

namespace OHOS {
namespace NetManagerStandard {
namespace {
constexpr uint32_t WAIT_FOR_SERVICE_TIME_S = 1;
constexpr uint32_t WAIT_FOR_SERVICE_TIME_MS = 500;
constexpr uint32_t MAX_GET_SERVICE_COUNT = 10;
std::condition_variable g_cv;
std::mutex g_mutexCv;
} // namespace
void NetworkShareLoadCallback::OnLoadSystemAbilitySuccess(
    int32_t systemAbilityId, const sptr<IRemoteObject> &remoteObject)
{
    NETMGR_EXT_LOG_D("OnLoadSystemAbilitySuccess systemAbilityId: [%{public}d]", systemAbilityId);
    std::unique_lock<std::mutex> lock(g_mutexCv);
    remoteObject_ = remoteObject;
    g_cv.notify_one();
}

void NetworkShareLoadCallback::OnLoadSystemAbilityFail(int32_t systemAbilityId)
{
    NETMGR_EXT_LOG_D("OnLoadSystemAbilityFail: [%{public}d]", systemAbilityId);
    loadSAFailed_ = true;
}

bool NetworkShareLoadCallback::IsFailed()
{
    return loadSAFailed_;
}

const sptr<IRemoteObject> &NetworkShareLoadCallback::GetRemoteObject() const
{
    return remoteObject_;
}

NetworkShareClient::NetworkShareClient()
    : networkShareService_(nullptr), deathRecipient_(nullptr), callback_(nullptr), nearlinkCallback_(nullptr) {}

NetworkShareClient::~NetworkShareClient()
{
    isDestroyed_ = true;
    NETMGR_EXT_LOG_I("~NetworkShareClient : Destroy NetworkShareClient");
    sptr<INetworkShareService> proxy = GetProxy();
    if (proxy == nullptr) {
        return;
    }
 
    auto serviceRemote = proxy->AsObject();
    if (serviceRemote == nullptr) {
        return;
    }

    if (deathRecipient_) {
        serviceRemote->RemoveDeathRecipient(deathRecipient_);
    }
}

int32_t NetworkShareClient::StartSharing(const SharingIfaceType &type)
{
    NETMGR_EXT_LOG_I("NetworkShare StartSharing type= %{public}d", type);
    sptr<INetworkShareService> proxy = GetProxy();
    if (proxy == nullptr) {
        NETMGR_EXT_LOG_E("StartSharing proxy is nullptr");
        return NETMANAGER_EXT_ERR_GET_PROXY_FAIL;
    }
    return proxy->StartNetworkSharing(static_cast<int32_t>(type));
}

int32_t NetworkShareClient::StopSharing(const SharingIfaceType &type)
{
    NETMGR_EXT_LOG_I("NetworkShare StopSharing type= %{public}d", type);
    sptr<INetworkShareService> proxy = GetProxy();
    if (proxy == nullptr) {
        NETMGR_EXT_LOG_E("StopSharing proxy is nullptr");
        return NETMANAGER_EXT_ERR_GET_PROXY_FAIL;
    }
    return proxy->StopNetworkSharing(static_cast<int32_t>(type));
}

int32_t NetworkShareClient::IsSharingSupported(int32_t &supported)
{
    NETMGR_EXT_LOG_I("NetworkShare IsSharingSupported.");
    sptr<INetworkShareService> proxy = GetProxy();
    if (proxy == nullptr) {
        NETMGR_EXT_LOG_E("IsSharingSupported proxy is nullptr");
        return NETMANAGER_EXT_ERR_GET_PROXY_FAIL;
    }
    return proxy->IsNetworkSharingSupported(supported);
}

int32_t NetworkShareClient::IsSharing(int32_t &sharingStatus)
{
    NETMGR_EXT_LOG_I("NetworkShare IsSharing.");
    sptr<INetworkShareService> proxy = GetProxy();
    if (proxy == nullptr) {
        NETMGR_EXT_LOG_E("IsSharing proxy is nullptr");
        return NETMANAGER_EXT_ERR_GET_PROXY_FAIL;
    }
    return proxy->IsSharing(sharingStatus);
}

int32_t NetworkShareClient::RegisterSharingEvent(sptr<ISharingEventCallback> callback)
{
    NETMGR_EXT_LOG_I("NetworkShare RegisterSharingEvent.");
    // LCOV_EXCL_START
    if (callback == nullptr) {
        NETMGR_EXT_LOG_E("callback is null");
        return NETMANAGER_EXT_ERR_LOCAL_PTR_NULL;
    }
    // LCOV_EXCL_STOP
    sptr<INetworkShareService> proxy = GetProxy();
    if (proxy == nullptr) {
        NETMGR_EXT_LOG_E("RegisterSharingEvent proxy is nullptr");
        return NETMANAGER_EXT_ERR_GET_PROXY_FAIL;
    }
    int32_t ret = proxy->RegisterSharingEvent(callback);
    if (ret == NETMANAGER_EXT_SUCCESS) {
        NETMGR_EXT_LOG_D("RegisterSharingEvent success, save callback.");
        std::lock_guard lock(mutex_);
        callback_ = callback;
    }

    return ret;
}

int32_t NetworkShareClient::UnregisterSharingEvent(sptr<ISharingEventCallback> callback)
{
    NETMGR_EXT_LOG_I("NetworkShare UnregisterSharingEvent.");
    // LCOV_EXCL_START
    if (callback == nullptr) {
        NETMGR_EXT_LOG_E("callback is null");
        return NETMANAGER_EXT_ERR_LOCAL_PTR_NULL;
    }
    // LCOV_EXCL_STOP
    sptr<INetworkShareService> proxy = GetProxy();
    if (proxy == nullptr) {
        NETMGR_EXT_LOG_E("UnregisterSharingEvent proxy is nullptr");
        return NETMANAGER_EXT_ERR_GET_PROXY_FAIL;
    }
    int32_t ret = proxy->UnregisterSharingEvent(callback);
    if (ret == NETMANAGER_EXT_SUCCESS) {
        NETMGR_EXT_LOG_D("UnRegisterSharingEvent success, delete callback.");
        std::lock_guard lock(mutex_);
        callback_ = nullptr;
    }

    return ret;
}

int32_t NetworkShareClient::GetSharableRegexs(const SharingIfaceType &type, std::vector<std::string> &ifaceRegexs)
{
    NETMGR_EXT_LOG_I("NetworkShare GetSharableRegexs type= %{public}d.", type);
    sptr<INetworkShareService> proxy = GetProxy();
    if (proxy == nullptr) {
        NETMGR_EXT_LOG_E("GetSharableRegexs proxy is nullptr");
        return NETMANAGER_EXT_ERR_GET_PROXY_FAIL;
    }
    return proxy->GetSharableRegexs(static_cast<int32_t>(type), ifaceRegexs);
}

int32_t NetworkShareClient::GetSharingState(const SharingIfaceType &type, SharingIfaceState &state)
{
    NETMGR_EXT_LOG_I("NetworkShare GetSharingState type= %{public}d.", type);
    sptr<INetworkShareService> proxy = GetProxy();
    if (proxy == nullptr) {
        NETMGR_EXT_LOG_E("GetSharingState proxy is nullptr");
        return NETMANAGER_EXT_ERR_GET_PROXY_FAIL;
    }
    int32_t stateInt = static_cast<int32_t>(state);
    int32_t ret = proxy->GetSharingState(static_cast<int32_t>(type), stateInt);
    state = SharingIfaceState(stateInt);
    return ret;
}

int32_t NetworkShareClient::GetSharingIfaces(const SharingIfaceState &state, std::vector<std::string> &ifaces)
{
    NETMGR_EXT_LOG_I("NetworkShare GetSharingIfaces type= %{public}d.", state);
    sptr<INetworkShareService> proxy = GetProxy();
    if (proxy == nullptr) {
        NETMGR_EXT_LOG_E("GetSharingIfaces proxy is nullptr");
        return NETMANAGER_EXT_ERR_GET_PROXY_FAIL;
    }
    return proxy->GetNetSharingIfaces(static_cast<int32_t>(state), ifaces);
}

int32_t NetworkShareClient::GetStatsRxBytes(int32_t &bytes)
{
    NETMGR_EXT_LOG_I("NetworkShare GetStatsRxBytes.");
    sptr<INetworkShareService> proxy = GetProxy();
    if (proxy == nullptr) {
        NETMGR_EXT_LOG_E("GetStatsRxBytes proxy is nullptr");
        return NETMANAGER_EXT_ERR_GET_PROXY_FAIL;
    }
    return proxy->GetStatsRxBytes(bytes);
}

int32_t NetworkShareClient::GetStatsTxBytes(int32_t &bytes)
{
    NETMGR_EXT_LOG_I("NetworkShare GetStatsTxBytes.");
    sptr<INetworkShareService> proxy = GetProxy();
    if (proxy == nullptr) {
        NETMGR_EXT_LOG_E("GetStatsTxBytes proxy is nullptr");
        return NETMANAGER_EXT_ERR_GET_PROXY_FAIL;
    }
    return proxy->GetStatsTxBytes(bytes);
}

int32_t NetworkShareClient::GetStatsTotalBytes(int32_t &bytes)
{
    NETMGR_EXT_LOG_I("NetworkShare GetStatsTotalBytes.");
    sptr<INetworkShareService> proxy = GetProxy();
    if (proxy == nullptr) {
        NETMGR_EXT_LOG_E("GetStatsTotalBytes proxy is nullptr");
        return NETMANAGER_EXT_ERR_GET_PROXY_FAIL;
    }
    return proxy->GetStatsTotalBytes(bytes);
}

int32_t NetworkShareClient::SetConfigureForShare(bool enabled)
{
    NETMGR_EXT_LOG_I("SetConfigureForShare NetworkShare.");
    sptr<INetworkShareService> proxy = GetProxy();
    if (proxy == nullptr) {
        NETMGR_EXT_LOG_E("SetConfigureForShare proxy is nullptr");
        return NETMANAGER_EXT_ERR_GET_PROXY_FAIL;
    }
    return proxy->SetConfigureForShare(enabled);
}

int32_t NetworkShareClient::IsNearlinkIpShareSupported(const std::string &peerAddress, bool &supported)
{
    auto proxy = GetProxy();
    return proxy == nullptr ? NETMANAGER_EXT_ERR_GET_PROXY_FAIL :
        proxy->IsNearlinkIpShareSupported(peerAddress, supported);
}

int32_t NetworkShareClient::StartNearlinkGateway(const std::string &peerAddress)
{
    auto proxy = GetProxy();
    return proxy == nullptr ? NETMANAGER_EXT_ERR_GET_PROXY_FAIL : proxy->StartNearlinkGateway(peerAddress);
}

int32_t NetworkShareClient::StopNearlinkGateway()
{
    auto proxy = GetProxy();
    return proxy == nullptr ? NETMANAGER_EXT_ERR_GET_PROXY_FAIL : proxy->StopNearlinkGateway();
}

int32_t NetworkShareClient::StartNearlinkTerminal(const std::string &gatewayAddress)
{
    auto proxy = GetProxy();
    return proxy == nullptr ? NETMANAGER_EXT_ERR_GET_PROXY_FAIL : proxy->StartNearlinkTerminal(gatewayAddress);
}

int32_t NetworkShareClient::StopNearlinkTerminal()
{
    auto proxy = GetProxy();
    return proxy == nullptr ? NETMANAGER_EXT_ERR_GET_PROXY_FAIL : proxy->StopNearlinkTerminal();
}

int32_t NetworkShareClient::GetNearlinkIpShareStatus(NearlinkIpShareStatus &status)
{
    auto proxy = GetProxy();
    return proxy == nullptr ? NETMANAGER_EXT_ERR_GET_PROXY_FAIL : proxy->GetNearlinkIpShareStatus(status);
}

int32_t NetworkShareClient::RegisterNearlinkIpShareEvent(sptr<INearlinkIpShareEventCallback> callback)
{
    if (callback == nullptr) {
        return NETMANAGER_EXT_ERR_LOCAL_PTR_NULL;
    }
    auto proxy = GetProxy();
    if (proxy == nullptr) {
        return NETMANAGER_EXT_ERR_GET_PROXY_FAIL;
    }
    int32_t ret = proxy->RegisterNearlinkIpShareEvent(callback);
    if (ret == NETMANAGER_EXT_SUCCESS) {
        std::lock_guard lock(mutex_);
        nearlinkCallback_ = callback;
    }
    return ret;
}

int32_t NetworkShareClient::UnregisterNearlinkIpShareEvent(sptr<INearlinkIpShareEventCallback> callback)
{
    if (callback == nullptr) {
        return NETMANAGER_EXT_ERR_LOCAL_PTR_NULL;
    }
    auto proxy = GetProxy();
    if (proxy == nullptr) {
        return NETMANAGER_EXT_ERR_GET_PROXY_FAIL;
    }
    int32_t ret = proxy->UnregisterNearlinkIpShareEvent(callback);
    if (ret == NETMANAGER_EXT_SUCCESS) {
        std::lock_guard lock(mutex_);
        nearlinkCallback_ = nullptr;
    }
    return ret;
}

sptr<INetworkShareService> NetworkShareClient::GetProxy()
{
    std::lock_guard locker(mutex_);
    if (networkShareService_ != nullptr) {
        return networkShareService_;
    }
    sptr<ISystemAbilityManager> sam = SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if (sam == nullptr) {
        NETMGR_EXT_LOG_E("get SystemAbilityManager failed");
        return nullptr;
    }
    sptr<IRemoteObject> remote = sam->GetSystemAbility(COMM_NET_TETHERING_MANAGER_SYS_ABILITY_ID);
    if (remote == nullptr) {
        NETMGR_EXT_LOG_E("get Remote service failed");
        return nullptr;
    }

    deathRecipient_ = new NetshareDeathRecipient(*this);
    if (deathRecipient_ == nullptr) {
        NETMGR_EXT_LOG_E("deathRecipient_ is nullptr");
        return nullptr;
    }
    if (remote->IsProxyObject() && !remote->AddDeathRecipient(deathRecipient_)) {
        NETMGR_EXT_LOG_E("add death recipient failed");
        return nullptr;
    }
    networkShareService_ = iface_cast<INetworkShareService>(remote);
    if (networkShareService_ == nullptr) {
        NETMGR_EXT_LOG_E("get Remote service proxy failed");
        if (remote->IsProxyObject() && deathRecipient_ != nullptr) {
            remote->RemoveDeathRecipient(deathRecipient_);
        }
        deathRecipient_ = nullptr;
        return nullptr;
    }
    return networkShareService_;
}

void NetworkShareClient::RecoverCallback()
{
    uint32_t count = 0;
    while (GetProxy() == nullptr && count < MAX_GET_SERVICE_COUNT) {
        if (isDestroyed_) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(WAIT_FOR_SERVICE_TIME_MS));
        count++;
    }
    if (isDestroyed_) {
        return;
    }
    auto proxy = GetProxy();
    NETMGR_EXT_LOG_D("Get proxy %{public}s, count: %{public}u", proxy == nullptr ? "failed" : "success", count);
    sptr<ISharingEventCallback> localCallback;
    sptr<INearlinkIpShareEventCallback> localNearlinkCallback;
    {
        std::lock_guard lock(mutex_);
        localCallback = callback_;
        localNearlinkCallback = nearlinkCallback_;
    }
    if (proxy != nullptr && localCallback != nullptr) {
        int32_t ret = proxy->RegisterSharingEvent(localCallback);
        NETMGR_EXT_LOG_D("Register result %{public}d", ret);
    }
    if (proxy != nullptr && localNearlinkCallback != nullptr) {
        int32_t ret = proxy->RegisterNearlinkIpShareEvent(localNearlinkCallback);
        NETMGR_EXT_LOG_D("Register NearLink result %{public}d", ret);
    }
}

void NetworkShareClient::OnRemoteDied(const wptr<IRemoteObject> &remote)
{
    NETMGR_EXT_LOG_I("NetworkShareClient:OnRemoteDied enter");
    if (remote == nullptr) {
        NETMGR_EXT_LOG_E("remote object is nullptr");
        return;
    }
    sptr<ISharingEventCallback> localCallback;
    sptr<INearlinkIpShareEventCallback> localNearlinkCallback;
    {
        std::lock_guard lock(mutex_);
        if (networkShareService_ == nullptr) {
            NETMGR_EXT_LOG_E("networkShareService_ is nullptr");
            return;
        }
        sptr<IRemoteObject> local = networkShareService_->AsObject();
        if (local != remote.promote()) {
            NETMGR_EXT_LOG_E("proxy and stub is not same remote object");
            return;
        }
        local->RemoveDeathRecipient(deathRecipient_);
        networkShareService_ = nullptr;
        localCallback = callback_;
        localNearlinkCallback = nearlinkCallback_;
    }

    std::thread([this]() { this->RestartNetTetheringManagerSysAbility(); }).detach();

    if (localCallback != nullptr || localNearlinkCallback != nullptr) {
        NETMGR_EXT_LOG_D("on remote died recover callback");
        std::thread t([this]() {
            RecoverCallback();
        });
        std::string threadName = "networkshareRecoverCallback";
        pthread_setname_np(t.native_handle(), threadName.c_str());
        t.detach();
    }
}

void NetworkShareClient::RestartNetTetheringManagerSysAbility()
{
    for (uint32_t i = 0; i < MAX_GET_SERVICE_COUNT; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(WAIT_FOR_SERVICE_TIME_S));
        if (isDestroyed_) {
            NETMGR_EXT_LOG_E("Restart NetTetheringManager destroyed.");
            return;
        }
        sptr<INetworkShareService> proxy = GetProxy();
        if (proxy) {
            NETMGR_EXT_LOG_I("Restart NetTetheringManager success.");
            return;
        }
    }
    NETMGR_EXT_LOG_E("Restart NetTetheringManager failed.");
}
} // namespace NetManagerStandard
} // namespace OHOS
