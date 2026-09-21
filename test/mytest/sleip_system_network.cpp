/* Copyright (c) 2026 Huawei Device Co., Ltd. Licensed under the Apache License, Version 2.0. */
#include "networkshare_client.h"
#include "net_conn_client.h"
#include "accesstoken_kit.h"
#include "nativetoken_kit.h"
#include "token_setproc.h"
#include <cstdio>
#include <cstring>

using namespace OHOS::NetManagerStandard;
extern "C" int SleipSystemNetwork(int argc, char **argv)
{
    if (argc < 3) return 2;
    const char *permissions[] = {"ohos.permission.CONNECTIVITY_INTERNAL", "ohos.permission.GET_NETWORK_INFO",
        "ohos.permission.ACCESS_NEARLINK"};
    NativeTokenInfoParams info{};
    info.permsNum = 3; info.perms = permissions; info.processName = "sleip_netshare_stage3"; info.aplStr = "system_core";
    uint64_t token = GetAccessTokenId(&info);
    if (!token || SetSelfTokenID(token) != 0 ||
        OHOS::Security::AccessToken::AccessTokenKit::ReloadNativeTokenInfo() != 0) return 1;
    auto &client = NetworkShareClient::GetInstance();
    int32_t ret = -1;
    if (argc == 5 && strcmp(argv[2], "start") == 0) {
        if (strcmp(argv[3], "G") == 0) ret = client.StartNearlinkGatewayWithMode(argv[4], 3);
        else if (strcmp(argv[3], "A") == 0) ret = client.StartNearlinkTerminalWithMode(argv[4], 3);
    } else if (argc == 4 && strcmp(argv[2], "stop") == 0) {
        if (strcmp(argv[3], "G") == 0) ret = client.StopNearlinkGateway();
        else if (strcmp(argv[3], "A") == 0) ret = client.StopNearlinkTerminal();
    } else if (argc == 3 && strcmp(argv[2], "status") == 0) {
        NearlinkIpShareStatus status;
        ret = client.GetNearlinkIpShareStatus(status);
        if (ret == 0) {
            NetHandle selected;
            int32_t defaultRet = NetConnClient::GetInstance().GetDefaultNet(selected);
            printf("state=%d mode=%d generation=%llu sequence=%llu netId=%d defaultRet=%d defaultNetId=%d\n",
                static_cast<int>(status.state), status.selectedMode,
                static_cast<unsigned long long>(status.generation), static_cast<unsigned long long>(status.sequence),
                status.netId, defaultRet, defaultRet == 0 ? selected.GetNetId() : -1);
            for (auto entry : {std::make_pair("IPv4", &status.ipv4), std::make_pair("IPv6", &status.ipv6)}) {
                const auto &family = *entry.second;
                printf("family=%s phase=%d configuration=%d external=%d validation=%d error=%s:%d\n", entry.first,
                    family.phase, family.configurationAvailable, family.externalAvailable, family.validation,
                    family.error.stage.c_str(), family.hasError ? family.error.code : 0);
                for (const auto &a : family.addresses)
                    printf("address=%s/%u scope=%u preferred=%u valid=%u\n", a.address.c_str(), a.prefixLength,
                        a.scopeId, a.preferredLifetime, a.validLifetime);
                for (const auto &r : family.routes)
                    printf("route=%s/%u via=%s scope=%u\n", r.destination.c_str(), r.prefixLength,
                        r.gateway.c_str(), r.scopeId);
                for (const auto &d : family.dns)
                    printf("dns=%s source=%u lifetime=%u lifetimeKnown=%d\n", d.address.c_str(), d.source, d.lifetime, d.lifetimeKnown);
            }
        }
    }
    printf("SYSTEM_NETWORK command=%s ret=%d accepted_only=%d\n", argv[2], ret, strcmp(argv[2], "start") == 0);
    return ret == 0 ? 0 : 1;
}
