"""Execute production prefix/RA reconciliation; substitute syscalls, Netsys and RA socket I/O."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
src = repo / 'services/networksharemanager'
production = (src / 'src/nearlink_ipv6_runtime.cpp').read_text()
prefix = production[production.index('bool Prefix('):production.index('bool NearlinkIpv6Runtime::Set(')]
advertise = production[production.index('bool NearlinkIpv6Runtime::Advertise('):production.index('bool NearlinkIpv6Runtime::Cleanup(')]
with tempfile.TemporaryDirectory(prefix='p2-s3-gateway-') as directory:
    out = Path(directory)
    def put(name, text):
        (out / name).write_text(text, encoding='utf-8')
    put('router_advertisement_daemon.h', r'''
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#undef ERROR
#include <vector>
#include <string>
namespace OHOS::NetManagerStandard {
struct IpPrefix {in6_addr prefix{};uint32_t prefixesLength=0,validLifetime=0,preferredLifetime=0;};
struct RaParams {bool layer3_=false;std::string macAddr_;int mtu_=0;uint32_t routerLifetime_=0,rdnssLifetime_=0;
std::vector<IpPrefix> prefixes_;std::vector<in6_addr> dnses_;};
class RouterAdvertisementDaemon {public:
inline static int starts=0;inline static std::vector<RaParams> sent;RaParams params;
int Init(const char*){return 0;}int StartRa(){++starts;return 0;}void StopRa(){}
void BuildNewRa(const RaParams&p){params=p;}bool AdvertiseNow(){sent.push_back(params);return true;}
};
}
''')
    put('net_link_info.h', '''#pragma once
#include <string>
#include <vector>
namespace OHOS::NetManagerStandard {
struct Address {int family_=0;unsigned prefixlen_=0;std::string address_;};
struct Route {Address destination_;};struct NetLinkInfo {std::vector<Address> netAddrList_;std::vector<Route> routeList_;};
}
''')
    put('nearlink_ipv6_runtime.h', (src / 'include/nearlink_ipv6_runtime.h').read_text())
    advertise = advertise.replace('/system/etc/communication/netmanager_ext/network_share_config.cfg', 'config.txt')
    advertise = advertise.replace('/proc/net/if_inet6', 'if_inet6.txt')
    put('test.cpp', r'''
#include <memory>
#include <chrono>
#include <map>
#include <algorithm>
#include <fstream>
#include <cassert>
#include <cstring>
#include <cstdio>
#define private public
#include "nearlink_ipv6_runtime.h"
#undef private
namespace OHOS::NetManagerStandard {
class NetsysController {public:
inline static std::vector<std::string> removed;
static NetsysController &GetInstance(){static NetsysController value;return value;}
int NetworkAddRoute(int,const char*,const std::string&,const char*){return 0;}
int NetworkRemoveRoute(int,const char*,const std::string&s,const char*){removed.push_back(s);return 0;}
int DelInterfaceAddress(const char*,const std::string&s,int){removed.push_back(s);return 0;}
};
bool NearlinkIpv6Runtime::AddAddress(const std::string &a){addresses_.push_back(a);return true;}
namespace {constexpr const char* IFACE="sleip0";
''' + prefix + advertise + r'''
}
using namespace OHOS::NetManagerStandard;
int main(){
 NearlinkIpv6Runtime runtime;runtime.ifindex_=7;runtime.layer2_="02:11:22:33:44:55";
 assert(!runtime.Advertise(nullptr,false));
 {std::ofstream f("config.txt");f<<"share_support:true\nnearlink_ipv6_prefix:fd77:77:1::\r\nnearlink_ipv6_dns:fd77:77:1::53\r\n";}
 {std::ofstream f("if_inet6.txt");f<<"fd770077000100000000000000000001 07 40 00 00 sleip0\n";}
 assert(!runtime.Advertise(nullptr,false));assert(RouterAdvertisementDaemon::starts==1);
 runtime.advertisedAt_-=std::chrono::seconds(6);
 assert(runtime.Advertise(nullptr,false));assert(runtime.daemon_->params.routerLifetime_==0);
 NetLinkInfo up;Route def;def.destination_.family_=AF_INET6;up.routeList_.push_back(def);
 assert(runtime.Advertise(&up,true));assert(runtime.daemon_->params.routerLifetime_==180);
 assert(runtime.Advertise(&up,true));assert(RouterAdvertisementDaemon::starts==1);
 {std::ofstream f("config.txt");f<<"nearlink_ipv6_prefix:fd77:77:2::\nnearlink_ipv6_dns:fd77:77:2::53\n";}
 assert(!runtime.Advertise(&up,true));assert(runtime.retired_.size()==1);
 assert(runtime.daemon_->params.prefixes_.size()==2);
 assert(runtime.daemon_->params.prefixes_[1].preferredLifetime==0);
 assert(runtime.addresses_.size()==1); // old address survives while new DAD is pending
 runtime.advertisedAt_-=std::chrono::seconds(6);
 {std::ofstream f("if_inet6.txt");f<<"fd770077000200000000000000000001 07 40 00 00 sleip0\n";}
 assert(runtime.Advertise(&up,true));assert(runtime.addresses_.size()==2);
 runtime.retired_[0].until=std::chrono::steady_clock::now()-std::chrono::seconds(1);
 assert(runtime.Advertise(&up,true));assert(runtime.retired_.empty());assert(runtime.addresses_.size()==1);
 up.netAddrList_.push_back({AF_INET6,64,"fd77:77:2::10"});
 assert(!runtime.Advertise(&up,true));assert(runtime.daemon_->params.routerLifetime_==0);
 assert(runtime.prefix_.empty()); // upstream on-link /64 is never copied downstream
 {std::ofstream f("config.txt");f<<"nearlink_ipv6_prefix:fd77:77:3::\nnearlink_ipv6_prefix:fd77:77:4::\nnearlink_ipv6_dns:fd77::53\n";}
 assert(!runtime.Advertise(nullptr,false));assert(runtime.prefix_.empty());
 {std::ofstream f("config.txt");f<<"nearlink_ipv6_prefix:\nnearlink_ipv6_dns:\n";}
 assert(!runtime.Advertise(nullptr,false));assert(runtime.prefix_.empty());
 puts("gateway: static prefix, no-upstream restriction, single RA owner, overlap rejection, renumber/DNS withdrawal, retained old resources PASS");
}
''')
    exe = out / 'test.exe'
    subprocess.run(['g++', '-std=c++17', f'-I{out}', str(out / 'test.cpp'), '-o', str(exe), '-lws2_32'], check=True)
    subprocess.run([str(exe)], cwd=out, check=True)
