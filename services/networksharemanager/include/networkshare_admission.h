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
#ifndef NETWORKSHARE_ADMISSION_H
#define NETWORKSHARE_ADMISSION_H

#include <functional>
#include <mutex>
#include <set>
#include <string>

#include "net_manager_constants.h"
#include "net_manager_ext_constants.h"

namespace OHOS::NetManagerStandard {
// Demo policy: NearLink gateway and legacy downstreams share global NAT/DNS exclusively.
// Reservations cover asynchronous starts/stops, not just the last reported UI state.
class NetworkShareAdmission final {
public:
    static NetworkShareAdmission &GetInstance()
    {
        static NetworkShareAdmission instance;
        return instance;
    }

    bool AcquireLegacy(const std::string &owner)
    {
        std::lock_guard lock(mutex_);
        if (nearlink_) {
            return false;
        }
        legacyOwners_.insert(owner);
        return true;
    }

    void ReleaseLegacy(const std::string &owner)
    {
        std::lock_guard lock(mutex_);
        legacyOwners_.erase(owner);
    }

    bool HasLegacy(const std::string &owner)
    {
        std::lock_guard lock(mutex_);
        return legacyOwners_.count(owner) != 0;
    }

    bool AcquireNearlink()
    {
        std::lock_guard lock(mutex_);
        if (nearlink_ || !legacyOwners_.empty()) {
            return false;
        }
        nearlink_ = true;
        return true;
    }

    void ReleaseNearlink()
    {
        std::lock_guard lock(mutex_);
        nearlink_ = false;
    }

    // Serialize a global-resource syscall with admission. Late legacy callbacks must
    // not clear a new NearLink session. A failed syscall retains its cleanup reservation.
    int32_t LegacyResource(const std::string &resource, bool acquire, const std::function<int32_t()> &operation)
    {
        std::lock_guard lock(mutex_);
        if (nearlink_) {
            return acquire ? NETMANAGER_EXT_ERR_OPERATION_FAILED : NETMANAGER_EXT_SUCCESS;
        }
        if (acquire) {
            legacyOwners_.insert(resource);
        }
        int32_t result = operation();
        if (!acquire && result == NETMANAGER_EXT_SUCCESS) {
            legacyOwners_.erase(resource);
        }
        return result;
    }

private:
    NetworkShareAdmission() = default;
    std::mutex mutex_;
    std::set<std::string> legacyOwners_;
    bool nearlink_{false};
};
} // namespace OHOS::NetManagerStandard
#endif // NETWORKSHARE_ADMISSION_H
