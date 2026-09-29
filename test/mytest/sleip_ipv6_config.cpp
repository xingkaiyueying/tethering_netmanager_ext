/* Copyright (c) 2026 Huawei Device Co., Ltd. Licensed under the Apache License, Version 2.0. */
#include "router_advertisement_daemon.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <net/if.h>
#include <sys/wait.h>
#include <vector>

using namespace OHOS::NetManagerStandard;
namespace {
int Ip(const std::vector<std::string> &args)
{
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        std::vector<char *> argv{const_cast<char *>("ip")};
        for (const auto &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr); execvp("ip", argv.data()); _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}
// The probe owns only sleip0 and restores every setting it changed. No global sysctl writes.
class InterfaceOwner {
public:
    InterfaceOwner() : index(if_nametoindex("sleip0")) {}
    bool Token(const std::string &value)
    {
        FILE *pipe = popen("ip token list dev sleip0", "r");
        if (!pipe) return false;
        char line[256]{}; bool read = fgets(line, sizeof(line), pipe) != nullptr;
        int result = pclose(pipe); std::string label;
        std::istringstream input(line); input >> label >> token;
        in6_addr parsed{};
        if (!read || result != 0 || label != "token" || inet_pton(AF_INET6, token.c_str(), &parsed) != 1) return false;
        haveToken = true;
        return Ip({"token", "set", value, "dev", "sleip0"}) == 0;
    }
    bool Set(const std::string &name, const std::string &value)
    {
        std::string path = "/proc/sys/net/ipv6/conf/sleip0/" + name;
        std::ifstream input(path); std::string old;
        if (!(input >> old)) return false;
        if (!saved.count(path)) saved[path] = old;
        std::ofstream output(path); output << value; output.flush(); return bool(output);
    }
    bool Flags()
    {
        int fd = socket(AF_INET, SOCK_DGRAM, 0); if (fd < 0) return false;
        ifreq request{}; strcpy(request.ifr_name, "sleip0");
        bool ok = ioctl(fd, SIOCGIFFLAGS, &request) == 0;
        if (ok) {
            flags = request.ifr_flags; haveFlags = true;
            request.ifr_flags = (request.ifr_flags | IFF_MULTICAST | IFF_UP) & ~IFF_NOARP;
            ok = ioctl(fd, SIOCSIFFLAGS, &request) == 0;
        }
        close(fd); return ok;
    }
    ~InterfaceOwner()
    {
        if (if_nametoindex("sleip0") != index) return;
        for (const auto &address : addresses) (void)Ip({"-6", "addr", "del", address, "dev", "sleip0"});
        if (haveToken) (void)Ip({"token", "set", token, "dev", "sleip0"});
        for (const auto &item : saved) { std::ofstream out(item.first); out << item.second; }
        if (haveFlags) {
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd >= 0) { ifreq request{}; strcpy(request.ifr_name, "sleip0"); request.ifr_flags = flags;
                (void)ioctl(fd, SIOCSIFFLAGS, &request); close(fd); }
        }
    }
    bool Add(const std::string &address)
    {
        if (HasAddress(address)) return true;
        if (Ip({"-6", "addr", "add", address, "dev", "sleip0"}) != 0) return false;
        addresses.push_back(address); return true;
    }
    void KeepAddresses() { addresses.clear(); }
private:
    bool HasAddress(const std::string &address) const
    {
        const std::string text = address.substr(0, address.find('/'));
        in6_addr expected{};
        if (inet_pton(AF_INET6, text.c_str(), &expected) != 1) return false;
        ifaddrs *head = nullptr;
        if (getifaddrs(&head) != 0) return false;
        bool found = false;
        for (auto item = head; item != nullptr && !found; item = item->ifa_next) {
            if (item->ifa_addr == nullptr || item->ifa_addr->sa_family != AF_INET6 ||
                strcmp(item->ifa_name, "sleip0") != 0) continue;
            const auto *actual = reinterpret_cast<const sockaddr_in6 *>(item->ifa_addr);
            found = memcmp(&actual->sin6_addr, &expected, sizeof(expected)) == 0;
        }
        freeifaddrs(head);
        return found;
    }
    std::map<std::string, std::string> saved;
    std::vector<std::string> addresses;
    short flags{0}; bool haveFlags{false};
    unsigned index{0}; std::string token; bool haveToken{false};
};
bool Number(const char *text, uint32_t &out)
{
    char *end{}; errno = 0; unsigned long long value = strtoull(text, &end, 10);
    if (errno || !*text || *end || value > UINT32_MAX) return false;
    out = value; return true;
}

int RaFailure(const char *stage, int code)
{
    fprintf(stderr, "S2_RA_FAILED stage=%s code=%d\n", stage, code);
    fflush(stderr);
    return code;
}

}

extern "C" int SleipIpv6Terminal(int argc, char **argv)
{
    // ipv6-terminal A_LLA SLAAC_TOKEN RUN_SECONDS; run before the DHCP probe, after CHANNEL_READY.
    in6_addr lla{}, token{}; uint32_t seconds;
    if (argc != 5 || !if_nametoindex("sleip0") || inet_pton(AF_INET6, argv[2], &lla) != 1 ||
        !IN6_IS_ADDR_LINKLOCAL(&lla) || inet_pton(AF_INET6, argv[3], &token) != 1 ||
        IN6_IS_ADDR_UNSPECIFIED(&token) || !Number(argv[4], seconds) || !seconds || seconds > 3600) return 2;
    for (size_t i = 0; i < 8; ++i) if (token.s6_addr[i]) return 2;
    InterfaceOwner owner;
    if (!owner.Flags() || !owner.Set("disable_ipv6", "0") || !owner.Set("forwarding", "0") ||
        !owner.Set("accept_ra", "2") || !owner.Set("autoconf", "1") || !owner.Set("use_tempaddr", "0") ||
        !owner.Set("accept_dad", "1") || !owner.Set("dad_transmits", "1") ||
        !owner.Set("router_solicitations", "6") || !owner.Set("router_solicitation_interval", "1") ||
        !owner.Token(argv[3]) || !owner.Add(std::string(argv[2]) + "/64")) return 1;
    unsigned index = if_nametoindex("sleip0");
    printf("S2_TERMINAL_PREPARED ifindex=%u; start DHCP dual probe in another shell; inspect actual DAD/SLAAC\n", index);
    fflush(stdout);
    for (uint32_t i = 0; i < seconds && if_nametoindex("sleip0") == index; ++i) sleep(1);
    return 0;
}

// Run after NearLink reports CHANNEL_READY. This is a native S2 fixture, not a system-network acceptance entry.
extern "C" int SleipIpv6Ra(int argc, char **argv)
{
    // ipv6-ra PREFIX DNS G_LLA G_LAYER2 ROUTER_LIFE PREFERRED VALID DNS_LIFE RUN_SECONDS
    if (argc != 11) return RaFailure("arguments", 2);
    if (if_nametoindex("sleip0") == 0) return RaFailure("interface", 2);
    in6_addr prefix{}, dns{}, lla{};
    uint32_t router, preferred, valid, dnsLife, seconds;
    if (inet_pton(AF_INET6, argv[2], &prefix) != 1 || inet_pton(AF_INET6, argv[3], &dns) != 1 ||
        inet_pton(AF_INET6, argv[4], &lla) != 1 || !IN6_IS_ADDR_LINKLOCAL(&lla))
        return RaFailure("addresses", 2);
    if (!Number(argv[6], router) || router > 9000 || !Number(argv[7], preferred) ||
        !Number(argv[8], valid) || !Number(argv[9], dnsLife) || !Number(argv[10], seconds) ||
        seconds > 3600 || !seconds) return RaFailure("lifetimes", 2);
    if (preferred > valid) return RaFailure("preferred_gt_valid", 2);
    if ((prefix.s6_addr[0] & 0xfe) != 0xfc && (prefix.s6_addr[0] & 0xe0) != 0x20)
        return RaFailure("prefix_scope", 2);
    for (size_t i = 8; i < 16; ++i) {
        if (prefix.s6_addr[i]) return RaFailure("prefix_length", 2);
    }
    unsigned values[6]; char tail;
    if (sscanf(argv[5], "%2x:%2x:%2x:%2x:%2x:%2x%c", &values[0], &values[1], &values[2], &values[3],
        &values[4], &values[5], &tail) != 6 || strlen(argv[5]) != 17) return RaFailure("layer2", 2);
    InterfaceOwner owner;
    if (!owner.Flags() || !owner.Set("disable_ipv6", "0") || !owner.Set("forwarding", "1") ||
        !owner.Set("accept_ra", "0") || !owner.Set("accept_dad", "1") || !owner.Set("dad_transmits", "1") ||
        !owner.Add(std::string(argv[4]) + "/64")) return RaFailure("interface_prepare", 1);
    sleep(2); // Kernel DAD outcome is subsequently enforced at the NearLink TUN transmit boundary.
    auto daemon = std::make_shared<RouterAdvertisementDaemon>();
    if (daemon->Init("sleip0") != 0) return RaFailure("daemon_init", 1);
    RaParams params;
    params.layer3_ = true; params.mtu_ = 1500; params.macAddr_ = argv[5]; params.routerLifetime_ = router;
    params.rdnssLifetime_ = dnsLife; params.dnses_.push_back(dns);
    IpPrefix p; p.prefix = prefix; p.prefixesLength = 64; p.preferredLifetime = preferred; p.validLifetime = valid;
    params.prefixes_.push_back(p); daemon->BuildNewRa(params);
    if (daemon->StartRa() != 0) return RaFailure("daemon_start", 1);
    sleep(3); // Advertise the authorized prefix before the gateway address starts DAD.
    prefix.s6_addr[15] = 1; char gateway[INET6_ADDRSTRLEN]{};
    inet_ntop(AF_INET6, &prefix, gateway, sizeof(gateway));
    if (!owner.Add(std::string(gateway) + "/64")) {
        daemon->StopRa();
        return RaFailure("gateway_address", 1);
    }
    printf("S2_RA_RUNNING prefix=%s/64 dns=%s gateway=%s "
        "first_source_confirmation=REQUIRED_WITHIN_60S external_validation=NOT_RUN\n",
        argv[2], argv[3], gateway);
    fflush(stdout);
    const unsigned index = if_nametoindex("sleip0");
    for (uint32_t i = 0; i < seconds && if_nametoindex("sleip0") == index; ++i) sleep(1);
    daemon->StopRa();
    // Confirm the new terminal source while RA is running; deletion may continue after return.
    // Keep successful gateway addresses until the NearLink round removes sleip0; a later
    // RA fixture in the same generation reuses the existing LLA and adds its new prefix.
    if (if_nametoindex("sleip0") == index) {
        owner.KeepAddresses();
        printf("S2_RA_STOPPED prefix=%s/64 gateway=%s gateway_retained_until_interface_cleanup=1\n",
            argv[2], gateway);
        fflush(stdout);
    }
    return 0;
}
