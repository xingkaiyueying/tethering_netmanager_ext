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

#include "netshare_observer_wrapper.h"

#include "constant.h"
#include "module_template.h"
#include "net_manager_constants.h"
#include "netmanager_ext_log.h"
#include "networkshare_client.h"
#include "networkshare_constants.h"

namespace OHOS {
namespace NetManagerStandard {
NetShareObserverWrapper::NetShareObserverWrapper()
{
    observer_ = new NetShareCallbackObserver();
    nearlinkObserver_ = new NearlinkIpShareCallbackObserver();
    manager_ = std::make_shared<EventManager>();
}

NetShareObserverWrapper::~NetShareObserverWrapper() = default;

napi_value NetShareObserverWrapper::On(napi_env env, napi_callback_info info,
                                       const std::initializer_list<std::string_view> &events, bool asyncCallback)
{
    size_t paramsCount = MAX_PARAM_NUM;
    napi_value params[MAX_PARAM_NUM] = {nullptr};
    NAPI_CALL(env, napi_get_cb_info(env, info, &paramsCount, params, nullptr, nullptr));
    if (paramsCount != PARAM_OPTIONS_AND_CALLBACK || NapiUtils::GetValueType(env, params[ARG_INDEX_0]) != napi_string ||
        NapiUtils::GetValueType(env, params[ARG_INDEX_1]) != napi_function) {
        NETMANAGER_EXT_LOGE("on off once interface para: [string, function]");
        napi_throw_error(env, std::to_string(NETMANAGER_EXT_ERR_PARAMETER_ERROR).c_str(), "Parameter error");
        return NapiUtils::GetUndefined(env);
    }

    const std::string event = NapiUtils::GetStringFromValueUtf8(env, params[ARG_INDEX_0]);
    NETMANAGER_EXT_LOGI("NetworkShare RegisterSharingEvent event = %{public}s", event.c_str());
    manager_->AddListener(env, event, params[ARG_INDEX_1], false, asyncCallback);
    bool isNearlink = event == EVENT_NEARLINK_IPSHARE_STATE_CHANGE;
    auto ret = isNearlink ? RegisterNearlink() : Register();
    if (ret == NETMANAGER_EXT_SUCCESS) {
        if (isNearlink) {
            nearlinkRegisted_ = true;
        } else {
            registed_ = true;
        }
    } else {
        manager_->DeleteListener(event, params[ARG_INDEX_1]);
        NETMANAGER_EXT_LOGE("RegisterSharingEvent error = %{public}d", ret);
        NetBaseErrorCodeConvertor convertor;
        std::string errorMsg = convertor.ConvertErrorCode(ret);
        napi_throw_error(env, std::to_string(ret).c_str(), errorMsg.c_str());
    }

    return NapiUtils::GetUndefined(env);
}

napi_value NetShareObserverWrapper::Off(napi_env env, napi_callback_info info,
                                        const std::initializer_list<std::string_view> &events, bool asyncCallback)
{
    napi_value thisVal = nullptr;
    size_t paramsCount = MAX_PARAM_NUM;
    napi_value params[MAX_PARAM_NUM] = {nullptr};
    NAPI_CALL(env, napi_get_cb_info(env, info, &paramsCount, params, &thisVal, nullptr));

    if ((paramsCount != PARAM_JUST_OPTIONS && paramsCount != PARAM_OPTIONS_AND_CALLBACK) ||
        NapiUtils::GetValueType(env, params[ARG_INDEX_0]) != napi_string) {
        NETMANAGER_EXT_LOGE("on off once interface para: [string, function?]");
        napi_throw_error(env, std::to_string(NETMANAGER_EXT_ERR_PARAMETER_ERROR).c_str(), "Parameter error");
        return NapiUtils::GetUndefined(env);
    }

    if (paramsCount == PARAM_OPTIONS_AND_CALLBACK &&
        NapiUtils::GetValueType(env, params[ARG_INDEX_1]) != napi_function) {
        NETMANAGER_EXT_LOGE("on off once interface para: [string, function]");
        return NapiUtils::GetUndefined(env);
    }

    const std::string event = NapiUtils::GetStringFromValueUtf8(env, params[ARG_INDEX_0]);
    NETMANAGER_EXT_LOGI("NetworkShare UnRegisterSharingEvent event = %{public}s", event.c_str());
    auto event_iterator = std::find(events.begin(), events.end(), event);
    if (event_iterator == events.end()) {
        return NapiUtils::GetUndefined(env);
    }

    if (paramsCount == PARAM_OPTIONS_AND_CALLBACK) {
        manager_->DeleteListener(event, params[ARG_INDEX_1]);
    } else {
        NETMANAGER_EXT_LOGI("SubEvent op=off_all kit=networkKit event=%{public}s", event.c_str());
        manager_->DeleteListener(event);
    }

    if (event == EVENT_NEARLINK_IPSHARE_STATE_CHANGE && nearlinkRegisted_ &&
        !manager_->HasEventListener(static_cast<std::string>(EVENT_NEARLINK_IPSHARE_STATE_CHANGE))) {
        int32_t result = DelayedSingleton<NetworkShareClient>::GetInstance()->UnregisterNearlinkIpShareEvent(
            nearlinkObserver_);
        if (result != NETMANAGER_EXT_SUCCESS) {
            napi_throw_error(env, std::to_string(result).c_str(), "Unregister NearLink event failed");
            return NapiUtils::GetUndefined(env);
        }
        nearlinkRegisted_ = false;
    }

    if (event != EVENT_NEARLINK_IPSHARE_STATE_CHANGE && registed_ && !HasSharingEventListener()) {
        int32_t result = DelayedSingleton<NetworkShareClient>::GetInstance()->UnregisterSharingEvent(observer_);
        if (result != NETMANAGER_EXT_SUCCESS) {
            NETMANAGER_EXT_LOGE("unregister result = %{public}d", result);
            NetBaseErrorCodeConvertor convertor;
            std::string errorMsg = convertor.ConvertErrorCode(result);
            napi_throw_error(env, std::to_string(result).c_str(), errorMsg.c_str());
            return NapiUtils::GetUndefined(env);
        }
        registed_ = false;
    }

    return NapiUtils::GetUndefined(env);
}

std::shared_ptr<EventManager> NetShareObserverWrapper::GetEventManager() const
{
    return manager_;
}

int32_t NetShareObserverWrapper::Register()
{
    if (registed_) {
        return NETMANAGER_EXT_SUCCESS;
    }

    int32_t result = DelayedSingleton<NetworkShareClient>::GetInstance()->RegisterSharingEvent(observer_);
    return result;
}

int32_t NetShareObserverWrapper::RegisterNearlink()
{
    if (nearlinkRegisted_) {
        return NETMANAGER_EXT_SUCCESS;
    }
    return DelayedSingleton<NetworkShareClient>::GetInstance()->RegisterNearlinkIpShareEvent(nearlinkObserver_);
}

bool NetShareObserverWrapper::HasSharingEventListener() const
{
    return manager_->HasEventListener(static_cast<std::string>(EVENT_SHARE_STATE_CHANGE)) ||
        manager_->HasEventListener(static_cast<std::string>(EVENT_IFACE_SHARE_STATE_CHANGE)) ||
        manager_->HasEventListener(static_cast<std::string>(EVENT_SHARE_UPSTREAM_CHANGE));
}
} // namespace NetManagerStandard
} // namespace OHOS
