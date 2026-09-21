/* Copyright (c) 2026 Huawei Device Co., Ltd. Licensed under the Apache License, Version 2.0. */
#include "nearlink_family_validation.h"
#include "net_conn_client.h"
#include <arpa/inet.h>
#include <cerrno>
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
bool Http204(int32_t netId, const addrinfo &address, const std::string &host, const std::string &path)
{
    int fd = socket(address.ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    auto check = [&]() {
        if (NetConnClient::GetInstance().BindSocket(fd, netId) != 0) return false;
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return false;
        int ret = connect(fd, address.ai_addr, address.ai_addrlen);
        if (ret != 0 && errno != EINPROGRESS) return false;
        pollfd wait{fd, POLLOUT, 0};
        int error = 0; socklen_t size = sizeof(error);
        if (poll(&wait, 1, 3000) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0)
            return false;
        if (fcntl(fd, F_SETFL, flags) != 0) return false;
        timeval timeout{3, 0};
        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ||
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) return false;
        std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
        size_t sent = 0;
        while (sent < request.size()) {
            auto count = send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
            if (count <= 0) return false;
            sent += count;
        }
        char response[256]{}; size_t used = 0;
        while (used + 1 < sizeof(response)) {
            auto count = recv(fd, response + used, sizeof(response) - used - 1, 0);
            if (count <= 0) return false;
            used += count;
            if (strstr(response, "\r\n")) {
                return strncmp(response, "HTTP/1.1 204 ", 13) == 0 || strncmp(response, "HTTP/1.0 204 ", 13) == 0;
            }
        }
        return false;
    };
    bool success = check(); close(fd); return success;
}
bool CheckFamily(int32_t netId, int family, const std::string &host, const std::string &port, const std::string &path)
{
    addrinfo hints{}; hints.ai_family = family; hints.ai_socktype = SOCK_STREAM;
    addrinfo *addresses = nullptr;
    queryparam query{}; query.qp_netid = netId; query.qp_type = QEURY_TYPE_NETSYS;
    if (getaddrinfo_ext(host.c_str(), port.c_str(), &hints, &addresses, &query) != 0) return false;
    bool success = false; unsigned tried = 0;
    for (auto entry = addresses; entry && tried < 2 && !success; entry = entry->ai_next) {
        if (entry->ai_family != family) continue;
        ++tried; success = Http204(netId, *entry, host, path);
    }
    if (addresses) freeaddrinfo(addresses);
    return success;
}
}
NearlinkFamilyValidation ValidateNearlinkFamilies(int32_t netId, bool ipv4, bool ipv6)
{
    NearlinkFamilyValidation result;
    if (netId < 0) return result;
    // Environment owner supplies an external endpoint. Missing input remains UNKNOWN, never a fabricated PASS.
    std::ifstream config("/system/etc/communication/netmanager_ext/nearlink_validation.conf");
    std::string host, path, extra; unsigned port = 0;
    if (!(config >> host >> port >> path) || (config >> extra) || host.size() > 253 ||
        port == 0 || port > 65535 || path.empty() || path[0] != '/' || path.size() > 512 ||
        host.find_first_of("\r\n\t /:") != std::string::npos || path.find_first_of("\r\n\t ") != std::string::npos)
        return result;
    if (ipv4) result.ipv4 = CheckFamily(netId, AF_INET, host, std::to_string(port), path) ? 2 : 3;
    if (ipv6) result.ipv6 = CheckFamily(netId, AF_INET6, host, std::to_string(port), path) ? 2 : 3;
    return result;
}
} // namespace OHOS::NetManagerStandard
