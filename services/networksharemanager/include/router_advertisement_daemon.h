/*
 * Copyright (c) 2024 Huawei Device Co., Ltd.
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

#ifndef ROUTER_ADVERTISEMENT_DAEMON_H
#define ROUTER_ADVERTISEMENT_DAEMON_H

#include "netmgr_ext_log_wrapper.h"
#include "router_advertisement_params.h"
#include <any>
#include <arpa/inet.h>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <random>
#include <securec.h>
#include <set>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include "ffrt_timer.h"

namespace OHOS {
namespace NetManagerStandard {

constexpr uint32_t HW_MAC_LENGTH = 6;
constexpr uint32_t IPV6_ADDR_LEN = 16;
constexpr uint32_t DEFAULT_RTR_INTERVAL_SEC = 600;
constexpr uint32_t DEFAULT_LIFETIME = 6 * DEFAULT_RTR_INTERVAL_SEC;

// www.rfc-editor.org/rfc/rfc4861#section-4.6
constexpr uint32_t UNITS_OF_OCTETS = 8;

/** ICMPv6 Ra package type, refer to
https://tools.ietf.org/html/rfc4861#section-13
*   Message name                            ICMPv6 Type
*   Router Solicitation                      133
*   Router Advertisement                     134
*  Option Name                               Type
*  Source Link-Layer Address                   1
*  Target Link-Layer Address                   2
*  Prefix Information                          3
*  MTU                                         5
*/
constexpr uint8_t ICMPV6_ND_ROUTER_SOLICIT_TYPE = 133;
constexpr uint8_t ICMPV6_ND_ROUTER_ADVERT_TYPE = 134;
constexpr uint8_t ND_OPTION_SLLA_TYPE = 1;
constexpr uint8_t ND_OPTION_PIO_TYPE = 3;
constexpr uint8_t ND_OPTION_MTU_TYPE = 5;
constexpr uint8_t ND_OPTION_RDNSS_TYPE = 25;

#pragma pack(1)
struct Icmpv6HeadSt {
    uint8_t type = ICMPV6_ND_ROUTER_ADVERT_TYPE;
    uint8_t code = 0;
    uint16_t checkSum = 0;
    uint8_t curHopLimit = 0;
    uint8_t flags = 0;
    uint16_t routerLifetime = 0;
    uint32_t reachLifetime = 0;
    uint32_t retransTimer = 0;
};
struct Icmpv6SllOpt {
    uint8_t type = ND_OPTION_SLLA_TYPE;
    uint8_t len = 0;
    uint8_t linkAddress[HW_MAC_LENGTH] = {};
};
struct Icmpv6MtuOpt {
    uint8_t type = ND_OPTION_MTU_TYPE;
    uint8_t len = 0;
    uint16_t res = 0;
    uint32_t mtu = 0;
};
struct Icmpv6PrefixInfoOpt {
    uint8_t type = ND_OPTION_PIO_TYPE;
    uint8_t len = 0;
    uint8_t prefixLen = 0;
    uint8_t flag = 0;
    uint32_t validLifetime = 0;
    uint32_t prefLifetime = 0;
    uint32_t res = 0;
    uint8_t prefix[IPV6_ADDR_LEN] = {};
};
struct Icmpv6RdnsOpt {
    uint8_t type = ND_OPTION_RDNSS_TYPE;
    uint8_t len = 0;
    uint16_t res = 0;
    uint32_t lifetime = 0;
    uint8_t dnsServer[0] = {};
};
#pragma pack()
class RouterAdvertisementDaemon : public std::enable_shared_from_this<RouterAdvertisementDaemon> {
public:
    RouterAdvertisementDaemon();
    ~RouterAdvertisementDaemon();
    int32_t Init(const std::string &ifaceName);
    int32_t StartRa();
    void StopRa();
    void ProcessSendRaPacket();
    RaParams GetDeprecatedRaParams(RaParams &oldRa, RaParams &newRa);
    void BuildNewRa(const RaParams &newRa);
    bool AdvertiseNow();

private:
    bool IsSocketValid();
    bool CreateRASocket();
    void CloseRaSocket();
    void RunRecvRsThread();
    void HupRaThread();
    bool MaybeSendRa(sockaddr_in6 &ra);
    void ResetRaRetryInterval();
    bool AssembleRaLocked();
    uint16_t PutRaHeader(uint8_t *raBuf);
    uint16_t PutRaSlla(uint8_t *raBuf, const std::string &mac);
    uint16_t PutRaMtu(uint8_t *raBuf, int32_t mtu);
    uint16_t PutRaPio(uint8_t *raBuf, IpPrefix &ipp);
    uint16_t PutRaRdnss(uint8_t *raBuf);

private:
    sockaddr_in6 dstIpv6Addr_ = {};
    int socket_ = -1;
    std::mutex mutex_;
    std::thread recvRsThread_;
    uint8_t sendRaTimes_ = 1;
    std::atomic<bool> stopRaThread_{false};
    uint8_t raPacket_[IPV6_MIN_MTU] = {};
    uint16_t raPacketLength_ = 0;
    std::shared_ptr<RaParams> raParams_;
    ffrt::shared_mutex sendRaFfrtQueueMutex_;
    std::shared_ptr<ffrt::queue> sendRaFfrtQueue_ = nullptr;
    ffrt::task_handle taskHandle_ = nullptr;
};

} // namespace NetManagerStandard
} // namespace OHOS
#endif // ROUTER_ADVERTISEMENT_DAEMON_H
