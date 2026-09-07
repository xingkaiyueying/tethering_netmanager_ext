/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
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

#ifndef NETWORKSHARE_SERVICE_H
#define NETWORKSHARE_SERVICE_H

#include <memory>
#include "common_event.h"
#include "common_event_data.h"
#include "common_event_manager.h"
#include "common_event_support.h"
#include "singleton.h"
#include "system_ability.h"
#include "errorcode_convertor.h"
#include "network_share_service_stub.h"
#include "networkshare_tracker.h"
#include "nearlink_ip_share_status.h"
#include "ffrt.h"

namespace OHOS {
namespace NetManagerStandard {
class NetworkShareService : public SystemAbility,
                            public NetworkShareServiceStub,
                            public std::enable_shared_from_this<NetworkShareService> {
    DECLARE_DELAYED_SINGLETON(NetworkShareService)
    DECLARE_SYSTEM_ABILITY(NetworkShareService)

    enum ServiceRunningState {
        STATE_STOPPED = 0,
        STATE_RUNNING,
    };

    class CommonEventSubscriber : public OHOS::EventFwk::CommonEventSubscriber {
    public:
        explicit CommonEventSubscriber(const EventFwk::CommonEventSubscribeInfo &subscribeInfo)
            : OHOS::EventFwk::CommonEventSubscriber(subscribeInfo) {};
        virtual void OnReceiveEvent(const EventFwk::CommonEventData &eventData) override;
    };

#ifdef SHARE_NOTIFICATION_ENABLE
    class WifiShareNtfSubscriber : public OHOS::EventFwk::CommonEventSubscriber {
    public:
        explicit WifiShareNtfSubscriber(const EventFwk::CommonEventSubscribeInfo &subscribeInfo)
            : OHOS::EventFwk::CommonEventSubscriber(subscribeInfo) {};
        virtual void OnReceiveEvent(const EventFwk::CommonEventData &eventData) override;
    };
#endif
public:
    /**
     * service start
     */
    void OnStart() override;

    /**
     * service stop
     */
    void OnStop() override;

    /**
     * is surpport share network
     */
    int32_t IsNetworkSharingSupported(int32_t &supported) override;

    /**
     * has shared network
     */
    int32_t IsSharing(int32_t &sharingStatus) override;

    /**
     * start network by type
     */
    int32_t StartNetworkSharing(int32_t type) override;

    /**
     * stop network by type
     */
    int32_t StopNetworkSharing(int32_t type) override;

    /**
     * get sharable regex
     */
    int32_t GetSharableRegexs(int32_t type, std::vector<std::string> &ifaceRegexs) override;

    /**
     * get sharing type
     */
    int32_t GetSharingState(int32_t type, int32_t &state) override;

    /**
     * get sharing ifaces
     */
    int32_t GetNetSharingIfaces(int32_t state, std::vector<std::string> &ifaces) override;

    /**
     * register callback
     */
    int32_t RegisterSharingEvent(const sptr<ISharingEventCallback>& callback) override;

    /**
     * unregister callback
     */
    int32_t UnregisterSharingEvent(const sptr<ISharingEventCallback>& callback) override;

    /**
     * get downlink data bytes
     */
    int32_t GetStatsRxBytes(int32_t &bytes) override;

    /**
     * get uplink data bytes
     */
    int32_t GetStatsTxBytes(int32_t &bytes) override;

    /**
     * get total data bytes
     */
    int32_t GetStatsTotalBytes(int32_t &bytes) override;

    /**
     * dump function
     */
    int32_t Dump(int32_t fd, const std::vector<std::u16string> &args) override;

    /**
     * set sysctl prop
     */
    int32_t SetConfigureForShare(bool enabled) override;

    int32_t IsNearlinkIpShareSupported(const std::string &peerAddress, bool &supported) override;
    int32_t StartNearlinkGateway(const std::string &peerAddress) override;
    int32_t StopNearlinkGateway() override;
    int32_t StartNearlinkTerminal(const std::string &gatewayAddress) override;
    int32_t StopNearlinkTerminal() override;
    int32_t GetNearlinkIpShareStatus(NearlinkIpShareStatus &status) override;

    int32_t GetBundleNameByUid(const int uid, std::string &bundleName);

protected:
    void OnAddSystemAbility(int32_t systemAbilityId, const std::string &deviceId) override;
    void OnRemoveSystemAbility(int32_t systemAbilityId, const std::string &deviceId) override;

private:
    bool Init();
    void GetDumpMessage(std::string &message);
    void GetSharingType(const SharingIfaceType &type, const std::string &typeContent, std::string &sharingType);
    void GetShareRegexsContent(const SharingIfaceType &type, std::string &shareRegexsContent);

    void OnNetSysRestart();
    static void DisAllowNetworkShareEventCallback(const char *key, const char *value, void *context);
    void SubscribeCommonEvent();
#ifdef SHARE_NOTIFICATION_ENABLE
    void SubscribeWifiShareNtfEvent();
#endif

private:
    ServiceRunningState state_ = ServiceRunningState::STATE_STOPPED;
    bool registerToService_ = false;
    std::shared_ptr<CommonEventSubscriber> commonEventSubscriber_ = nullptr;
#ifdef SHARE_NOTIFICATION_ENABLE
    std::shared_ptr<WifiShareNtfSubscriber> wifiShareNtfSubscriber_ = nullptr;
#endif
    bool hasSARemoved_ = false;
    int32_t setConfigTimes_ = 0;
    ffrt::mutex setConfigureMutex_;
    ffrt::mutex openFileMutex_;
    ffrt::mutex subscriberMutex_;
};
} // namespace NetManagerStandard
} // namespace OHOS
#endif // NETWORKSHARE_SERVICE_H
