/* Copyright (c) 2026 Huawei Device Co., Ltd. Licensed under the Apache License, Version 2.0. */
#ifndef NEARLINK_FAMILY_VALIDATION_H
#define NEARLINK_FAMILY_VALIDATION_H
#include <cstdint>
namespace OHOS::NetManagerStandard {
struct NearlinkFamilyValidation {
    int32_t ipv4{0}, ipv6{0}; // UNKNOWN=0, VALIDATED=2, FAILED=3
};
// Uses the system resolver for this netId and a configured external HTTP 204 endpoint.
// It does not prove default-network selection or replace TCP/UDP/HTTPS device acceptance.
NearlinkFamilyValidation ValidateNearlinkFamilies(int32_t netId, bool ipv4, bool ipv6);
} // namespace OHOS::NetManagerStandard
#endif
