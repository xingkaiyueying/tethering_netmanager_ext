/* Copyright (c) 2026 Huawei Device Co., Ltd. Licensed under the Apache License, Version 2.0. */
#include "nearlink_family_validation.h"
#include "net_conn_client.h"
#include "netmgr_ext_log_wrapper.h"
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace OHOS::NetManagerStandard {
namespace {
bool ProbeFailure(int32_t netId, int family, const char *stage, int32_t code)
{
    NETMGR_EXT_LOG_W("[NearlinkIpShare][Probe] netId=%{public}d family=%{public}d "
                     "stage=%{public}s code=%{public}d", netId, family, stage, code);
    return false;
}

bool ReadProbe(std::string &host, std::string &port, std::string &path, std::string &authority)
{
    std::ifstream config("/system/etc/netdetectionurl.conf");
    std::string line, url;
    bool seen = false;
    while (std::getline(config, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 13, "HttpProbeUrl:") != 0) continue;
        if (seen) return false;
        seen = true; url = line.substr(13);
    }
    // This probe supports direct HTTP 204 only. Unsupported input stays UNKNOWN.
    if (!seen || url.size() > 1024 || url.compare(0, 7, "http://") != 0 ||
        url.find_first_of("\r\n\t #") != std::string::npos) return false;
    auto end = url.find_first_of("/?", 7);
    authority = url.substr(7, end == std::string::npos ? end : end - 7);
    path = end == std::string::npos ? "/" : url.substr(end);
    if (path[0] == '?') path.insert(0, "/");
    if (authority.empty() || authority.find_first_of("@[]\\") != std::string::npos || path.size() > 512)
        return false;
    auto colon = authority.find(':');
    host = authority.substr(0, colon); port = colon == std::string::npos ? "80" : authority.substr(colon + 1);
    if (host.empty() || host.size() > 253 || port.empty() || port.size() > 5 ||
        port.find_first_not_of("0123456789") != std::string::npos) return false;
    unsigned number = 0;
    for (char digit : port) number = number * 10 + digit - '0';
    return number > 0 && number <= 65535;
}
bool Http204(int32_t netId, const addrinfo &address, const std::string &host, const std::string &path)
{
    int fd = socket(address.ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return ProbeFailure(netId, address.ai_family, "SOCKET", errno);
    auto started = std::chrono::steady_clock::now();
    auto addressText = [](const sockaddr *socketAddress, char *text, size_t length) {
        if (socketAddress == nullptr) return;
        if (socketAddress->sa_family == AF_INET) {
            (void)inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in *>(socketAddress)->sin_addr, text, length);
        } else if (socketAddress->sa_family == AF_INET6) {
            (void)inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6 *>(socketAddress)->sin6_addr, text, length);
        }
    };
    char remote[INET6_ADDRSTRLEN]{}, local[INET6_ADDRSTRLEN]{};
    addressText(address.ai_addr, remote, sizeof(remote));
    auto check = [&]() {
        int32_t bindRet = NetConnClient::GetInstance().BindSocket(fd, netId);
        if (bindRet != 0) return ProbeFailure(netId, address.ai_family, "BIND", bindRet);
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return false;
        int ret = connect(fd, address.ai_addr, address.ai_addrlen);
        if (ret != 0 && errno != EINPROGRESS) return ProbeFailure(netId, address.ai_family, "CONNECT", errno);
        pollfd wait{fd, POLLOUT, 0};
        int error = 0; socklen_t size = sizeof(error);
        int ready = poll(&wait, 1, 3000);
        if (ready <= 0) return ProbeFailure(netId, address.ai_family, "CONNECT_WAIT", ready == 0 ? ETIMEDOUT : errno);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0)
            return ProbeFailure(netId, address.ai_family, "CONNECT_STATUS", errno);
        if (error != 0) return ProbeFailure(netId, address.ai_family, "CONNECT_STATUS", error);
        if (fcntl(fd, F_SETFL, flags) != 0) return false;
        timeval timeout{3, 0};
        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ||
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) return false;
        std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
        size_t sent = 0;
        while (sent < request.size()) {
            auto count = send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
            if (count <= 0) return ProbeFailure(netId, address.ai_family, "HTTP_SEND", count == 0 ? EIO : errno);
            sent += count;
        }
        char response[256]{}; size_t used = 0;
        while (used + 1 < sizeof(response)) {
            auto count = recv(fd, response + used, sizeof(response) - used - 1, 0);
            if (count <= 0) return ProbeFailure(netId, address.ai_family, "HTTP_RECEIVE", count == 0 ? EIO : errno);
            used += count;
            if (strstr(response, "\r\n")) {
                bool valid = strncmp(response, "HTTP/1.1 204 ", 13) == 0 || strncmp(response, "HTTP/1.0 204 ", 13) == 0;
                return valid || ProbeFailure(netId, address.ai_family, "HTTP_STATUS", EPROTO);
            }
        }
        return ProbeFailure(netId, address.ai_family, "HTTP_HEADER", EMSGSIZE);
    };
    bool success = check();
    sockaddr_storage source{};
    socklen_t sourceLength = sizeof(source);
    if (getsockname(fd, reinterpret_cast<sockaddr *>(&source), &sourceLength) == 0) {
        addressText(reinterpret_cast<const sockaddr *>(&source), local, sizeof(local));
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    NETMGR_EXT_LOG_I("[NearlinkIpShare][ProbeSocket] netId=%{public}d family=%{public}d "
                     "remote=%{public}s local=%{public}s success=%{public}d elapsedMs=%{public}lld",
                     netId, address.ai_family, remote, local, success, static_cast<long long>(elapsed));
    close(fd);
    return success;
}
bool CheckFamily(int32_t netId, int family, const std::string &host, const std::string &port,
    const std::string &path, const std::string &authority)
{
    addrinfo hints{}; hints.ai_family = family; hints.ai_socktype = SOCK_STREAM;
    addrinfo *addresses = nullptr;
    queryparam query{}; query.qp_netid = netId; query.qp_type = QEURY_TYPE_NETSYS;
    int resolveRet = getaddrinfo_ext(host.c_str(), port.c_str(), &hints, &addresses, &query);
    if (resolveRet != 0) return ProbeFailure(netId, family, "DNS", resolveRet);
    bool success = false; unsigned tried = 0;
    for (auto entry = addresses; entry && tried < 2 && !success; entry = entry->ai_next) {
        if (entry->ai_family != family) continue;
        ++tried; success = Http204(netId, *entry, authority, path);
    }
    if (addresses) freeaddrinfo(addresses);
    if (tried == 0) return ProbeFailure(netId, family, "DNS_EMPTY", 0);
    return success;
}
}
NearlinkFamilyValidation ValidateNearlinkFamilies(int32_t netId, bool ipv4, bool ipv6)
{
    NearlinkFamilyValidation result;
    if (netId < 0) return result;
    std::string host, port, path, authority;
    if (!ReadProbe(host, port, path, authority)) return result;
    if (ipv4) result.ipv4 = CheckFamily(netId, AF_INET, host, port, path, authority) ? 2 : 3;
    if (ipv6) result.ipv6 = CheckFamily(netId, AF_INET6, host, port, path, authority) ? 2 : 3;
    NETMGR_EXT_LOG_I("[NearlinkIpShare][ProbeResult] netId=%{public}d ipv4=%{public}d ipv6=%{public}d",
                     netId, result.ipv4, result.ipv6);
    return result;
}
} // namespace OHOS::NetManagerStandard
