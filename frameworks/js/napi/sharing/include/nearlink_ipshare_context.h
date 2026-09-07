/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */
#ifndef NETMANAGER_EXT_NEARLINK_IPSHARE_CONTEXT_H
#define NETMANAGER_EXT_NEARLINK_IPSHARE_CONTEXT_H

#include "base_context.h"
#include "nearlink_ipshare_status.h"

namespace OHOS::NetManagerStandard {
class NearlinkIpShareContext final : public BaseContext {
public:
    NearlinkIpShareContext() = delete;
    explicit NearlinkIpShareContext(napi_env env, std::shared_ptr<EventManager> &manager);
    void ParseParams(napi_value *params, size_t paramsCount);

    const std::string &GetPeerAddress() const;
    void SetSupported(bool supported);
    bool IsSupported() const;
    void SetStatus(const NearlinkIpShareStatus &status);
    const NearlinkIpShareStatus &GetStatus() const;

private:
    std::string peerAddress_;
    bool supported_ {false};
    NearlinkIpShareStatus status_;
};
} // namespace OHOS::NetManagerStandard
#endif // NETMANAGER_EXT_NEARLINK_IPSHARE_CONTEXT_H
