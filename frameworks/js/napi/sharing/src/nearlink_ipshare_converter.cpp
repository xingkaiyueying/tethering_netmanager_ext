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
#include "nearlink_ipshare_converter.h"

#include <string>

#include "napi_utils.h"

namespace OHOS::NetManagerStandard {
namespace {
const char *RoleName(NearlinkIpShareRole role)
{
    switch (role) {
        case NearlinkIpShareRole::GATEWAY: return "GATEWAY";
        case NearlinkIpShareRole::TERMINAL: return "TERMINAL";
        default: return "NONE";
    }
}

const char *StateName(NearlinkIpShareState state)
{
    switch (state) {
        case NearlinkIpShareState::STARTING: return "STARTING";
        case NearlinkIpShareState::DISCOVERING: return "DISCOVERING";
        case NearlinkIpShareState::CONFIGURING: return "CONFIGURING";
        case NearlinkIpShareState::IFACE_READY: return "IFACE_READY";
        case NearlinkIpShareState::CHANNEL_READY: return "CHANNEL_READY";
        case NearlinkIpShareState::DHCP: return "DHCP";
        case NearlinkIpShareState::SERVING: return "SERVING";
        case NearlinkIpShareState::SERVING_NO_UPSTREAM: return "SERVING_NO_UPSTREAM";
        case NearlinkIpShareState::ACTIVE: return "ACTIVE";
        case NearlinkIpShareState::STOPPING: return "STOPPING";
        case NearlinkIpShareState::ERROR: return "ERROR";
        default: return "IDLE";
    }
}
const char *ModeName(int32_t mode) { return mode == 3 ? "DUAL_STACK" : "IPV4"; }
const char *PhaseName(int32_t phase)
{
    constexpr const char *NAMES[] = {"DISABLED", "CONFIGURING", "AVAILABLE", "FAILED"};
    return phase >= 0 && phase <= 3 ? NAMES[phase] : NAMES[0];
}
const char *ValidationName(int32_t value)
{
    constexpr const char *NAMES[] = {"UNKNOWN", "CHECKING", "VALIDATED", "FAILED"};
    return value >= 0 && value <= 3 ? NAMES[value] : NAMES[0];
}
void Put(napi_env env, napi_value object, const char *key, napi_value value)
{
    napi_set_named_property(env, object, key, value);
}
napi_value Number(napi_env env, uint32_t value) { return NapiUtils::CreateUint32(env, value); }
napi_value AddressToJs(napi_env env, const NearlinkIpShareAddress &address)
{
    auto value = NapiUtils::CreateObject(env);
    NapiUtils::SetStringPropertyUtf8(env, value, "address", address.address);
    Put(env, value, "prefixLength", Number(env, address.prefixLength));
    Put(env, value, "scopeId", Number(env, address.scopeId));
    Put(env, value, "origin", Number(env, address.origin));
    Put(env, value, "dadState", Number(env, address.dadState));
    Put(env, value, "preferredLifetime", Number(env, address.preferredLifetime));
    Put(env, value, "validLifetime", Number(env, address.validLifetime));
    return value;
}
napi_value RouteToJs(napi_env env, const NearlinkIpShareRoute &route)
{
    auto value = NapiUtils::CreateObject(env);
    NapiUtils::SetStringPropertyUtf8(env, value, "destination", route.destination);
    NapiUtils::SetStringPropertyUtf8(env, value, "gateway", route.gateway);
    Put(env, value, "prefixLength", Number(env, route.prefixLength));
    Put(env, value, "scopeId", Number(env, route.scopeId));
    Put(env, value, "lifetime", Number(env, route.lifetime));
    NapiUtils::SetBooleanProperty(env, value, "lifetimeKnown", route.lifetimeKnown);
    return value;
}
napi_value DnsToJs(napi_env env, const NearlinkIpShareDns &dns)
{
    auto value = NapiUtils::CreateObject(env);
    NapiUtils::SetStringPropertyUtf8(env, value, "address", dns.address);
    Put(env, value, "transportFamily", Number(env, dns.transportFamily));
    Put(env, value, "source", Number(env, dns.source));
    Put(env, value, "lifetime", Number(env, dns.lifetime));
    NapiUtils::SetBooleanProperty(env, value, "lifetimeKnown", dns.lifetimeKnown);
    return value;
}
template<class T, class F> napi_value ListToJs(napi_env env, const std::vector<T> &items, F convert)
{
    auto array = NapiUtils::CreateArray(env, items.size());
    for (size_t i = 0; i < items.size(); ++i) NapiUtils::SetArrayElement(env, array, i, convert(env, items[i]));
    return array;
}
napi_value FamilyToJs(napi_env env, const NearlinkIpShareFamilyStatus &family)
{
    auto value = NapiUtils::CreateObject(env);
    NapiUtils::SetStringPropertyUtf8(env, value, "phase", PhaseName(family.phase));
    NapiUtils::SetBooleanProperty(env, value, "configurationAvailable", family.configurationAvailable);
    NapiUtils::SetBooleanProperty(env, value, "externalAvailable", family.externalAvailable);
    NapiUtils::SetStringPropertyUtf8(env, value, "validation", ValidationName(family.validation));
    Put(env, value, "addresses", ListToJs(env, family.addresses, AddressToJs));
    Put(env, value, "routes", ListToJs(env, family.routes, RouteToJs));
    Put(env, value, "dns", ListToJs(env, family.dns, DnsToJs));
    if (family.hasError) {
        auto error = NapiUtils::CreateObject(env);
        Put(env, error, "plane", Number(env, family.error.plane));
        NapiUtils::SetStringPropertyUtf8(env, error, "stage", family.error.stage);
        Put(env, error, "family", Number(env, family.error.family));
        NapiUtils::SetInt32Property(env, error, "code", family.error.code);
        NapiUtils::SetBooleanProperty(env, error, "retryable", family.error.retryable);
        Put(env, value, "error", error);
    }
    return value;
}
} // namespace

napi_value NearlinkIpShareConverter::ToJs(napi_env env, const NearlinkIpShareStatus &status)
{
    napi_value value = NapiUtils::CreateObject(env);
    NapiUtils::SetStringPropertyUtf8(env, value, "role", RoleName(status.role));
    NapiUtils::SetStringPropertyUtf8(env, value, "state", StateName(status.state));
    NapiUtils::SetStringPropertyUtf8(env, value, "peerAddress", status.peerAddress);
    NapiUtils::SetStringPropertyUtf8(env, value, "ifaceName", status.ifaceName);
    NapiUtils::SetStringPropertyUtf8(env, value, "ipv4Address", status.ipv4Address);
    NapiUtils::SetBooleanProperty(env, value, "hasUpstream", status.hasUpstream);
    NapiUtils::SetStringPropertyUtf8(env, value, "errorStage", status.errorStage);
    NapiUtils::SetInt32Property(env, value, "errorCode", status.errorCode);
    NapiUtils::SetStringPropertyUtf8(env, value, "contextId", status.contextId);
    NapiUtils::SetStringPropertyUtf8(env, value, "generation", std::to_string(status.generation));
    NapiUtils::SetStringPropertyUtf8(env, value, "sequence", std::to_string(status.sequence));
    NapiUtils::SetStringPropertyUtf8(env, value, "requestedMode", ModeName(status.requestedMode));
    if (status.selectedMode != 0) NapiUtils::SetStringPropertyUtf8(env, value, "selectedMode", ModeName(status.selectedMode));
    if (!status.fallbackReason.empty()) NapiUtils::SetStringPropertyUtf8(env, value, "fallbackReason", status.fallbackReason);
    if (status.netId >= 0) NapiUtils::SetInt32Property(env, value, "netId", status.netId);
    NapiUtils::SetBooleanProperty(env, value, "serviceReady", status.serviceReady);
    Put(env, value, "ipv4", FamilyToJs(env, status.ipv4));
    Put(env, value, "ipv6", FamilyToJs(env, status.ipv6));
    return value;
}

napi_value NearlinkIpShareConverter::CapabilitiesToJs(napi_env env, const NearlinkIpShareCapabilities &capabilities)
{
    auto value = NapiUtils::CreateObject(env);
    NapiUtils::SetBooleanProperty(env, value, "identifierPresent", capabilities.identifierPresent);
    NapiUtils::SetInt32Property(env, value, "discoveryState", capabilities.discoveryState);
    auto modes = [env](const std::vector<int32_t> &source) {
        auto result = NapiUtils::CreateArray(env, source.size());
        for (size_t i = 0; i < source.size(); ++i) {
            NapiUtils::SetArrayElement(env, result, i, NapiUtils::CreateStringUtf8(env, ModeName(source[i])));
        }
        return result;
    };
    Put(env, value, "localModes", modes(capabilities.localModes));
    Put(env, value, "peerModes", modes(capabilities.peerModes));
    NapiUtils::SetBooleanProperty(env, value, "peerCapabilityKnown", capabilities.peerCapabilityKnown);
    return value;
}
} // namespace OHOS::NetManagerStandard
