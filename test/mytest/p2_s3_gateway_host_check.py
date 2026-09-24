"""Execute production prefix/RA reconciliation; substitute syscalls, Netsys and RA socket I/O."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
src = repo / 'services/networksharemanager'
production = (src / 'src/nearlink_ipv6_runtime.cpp').read_text()
prefix = production[production.index('constexpr const char *IFACE'):production.index('bool NearlinkIpv6Runtime::Set(')]
prefix = prefix.replace('/proc/net/if_inet6', 'if_inet6_upstream.txt')
advertise = production[production.index('bool NearlinkIpv6Runtime::Advertise('):production.index('bool NearlinkIpv6Runtime::Cleanup(')]
with tempfile.TemporaryDirectory(prefix='p2-s3-gateway-') as directory:
    out = Path(directory)
    def put(name, text):
        file = out / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(text, encoding='utf-8')
    put('arpa/inet.h', '''#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#undef ERROR
extern "C" int inet_pton(int,const char*,void*);
extern "C" const char* inet_ntop(int,const void*,char*,size_t);
''')
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
struct Route {Address destination_;};struct NetLinkInfo {std::string ifaceName_;std::vector<Address> netAddrList_;std::vector<Route> routeList_;};
}
''')
    put('nearlink_ipv6_runtime.h', (src / 'include/nearlink_ipv6_runtime.h').read_text())
    advertise = advertise.replace('/proc/net/if_inet6', 'if_inet6.txt')
    put('test.cpp', r'''
#include <memory>
#include <chrono>
#include <map>
#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <arpa/inet.h>
#define private public
#include "nearlink_ipv6_runtime.h"
#undef private
namespace OHOS::NetManagerStandard {
class NetsysController {public:
inline static std::vector<std::string> removed;inline static bool failRemove=false;
static NetsysController &GetInstance(){static NetsysController value;return value;}
int NetworkAddRoute(int,const char*,const std::string&,const char* nextHop){return strcmp(nextHop,"::")==0?-EINVAL:0;}
int NetworkRemoveRoute(int,const char*,const std::string&s,const char*){if(failRemove)return -1;removed.push_back(s);return 0;}
int DelInterfaceAddress(const char*,const std::string&s,int){if(failRemove)return -1;removed.push_back(s);return 0;}
};
bool NearlinkIpv6Runtime::AddAddress(const std::string &a){addresses_.push_back(a);return true;}
namespace {
''' + prefix + r'''
std::string routedInterface;
bool HasIpv6DefaultRouteOnInterface(const std::string &iface)
{
    return iface == routedInterface;
}
''' + advertise + r'''
}
using namespace OHOS::NetManagerStandard;
int main(){
 NearlinkIpv6Runtime runtime;runtime.ifindex_=7;runtime.layer2_="02:11:22:33:44:55";
 assert(!runtime.Advertise(nullptr,false));
 NetLinkInfo up;up.netAddrList_.push_back({AF_INET6,64,"2001:db8:10:20::1234"});
 Route def;def.destination_.family_=AF_INET6;up.routeList_.push_back(def);
 {std::ofstream f("if_inet6.txt");f<<"20010db800100021001122fffe334455 07 40 00 00 sleip0\n";}
 assert(!runtime.Advertise(&up,true));assert(RouterAdvertisementDaemon::starts==1);
 assert(runtime.prefix_=="2001:db8:10:21::");
 assert(runtime.gateway_=="2001:db8:10:21:11:22ff:fe33:4455");
 assert(runtime.daemon_->params.dnses_.size()==1);
 runtime.advertisedAt_-=std::chrono::seconds(6);
 assert(runtime.Advertise(&up,true));assert(runtime.daemon_->params.routerLifetime_==180);
 assert(runtime.Advertise(&up,true));assert(RouterAdvertisementDaemon::starts==1);
 up.netAddrList_.clear();up.netAddrList_.push_back({AF_INET6,64,"2001:db8:10:30::abcd"});
 assert(!runtime.Advertise(&up,true));assert(runtime.retired_.size()==1);
 assert(runtime.daemon_->params.prefixes_.size()==2);
 assert(runtime.daemon_->params.prefixes_[1].preferredLifetime==0);
 assert(runtime.addresses_.size()==1); // old address survives while new DAD is pending
 runtime.advertisedAt_-=std::chrono::seconds(6);
 {std::ofstream f("if_inet6.txt");f<<"20010db800100031001122fffe334455 07 40 00 00 sleip0\n";}
 assert(runtime.Advertise(&up,true));assert(runtime.addresses_.size()==2);
 runtime.retired_[0].until=std::chrono::steady_clock::now()-std::chrono::seconds(1);
 assert(runtime.Advertise(&up,true));assert(runtime.retired_.empty());assert(runtime.addresses_.size()==1);
 // A fifth prefix must withdraw old DNS/default routing even while all four slots are retained.
 for(const char* address : {"2001:db8:10:40::1","2001:db8:10:50::1","2001:db8:10:60::1","2001:db8:10:70::1"}) {
  up.netAddrList_[0].address_=address;runtime.Advertise(&up,true);
 }
 assert(runtime.retired_.size()==4 && runtime.prefix_.empty());
 assert(runtime.daemon_->params.routerLifetime_==0 && runtime.daemon_->params.dnses_.empty());
 runtime.Advertise(nullptr,false);assert(runtime.daemon_->params.routerLifetime_==0);
 for(auto &old:runtime.retired_)old.until=std::chrono::steady_clock::now()-std::chrono::seconds(1);
 NetsysController::failRemove=true;runtime.Advertise(&up,true);
 assert(!runtime.retired_.empty()); // failed resources are retained for cleanup, never silently dropped
 NetsysController::failRemove=false;runtime.Advertise(&up,true);
 assert(runtime.retired_.empty() && runtime.prefix_=="2001:db8:10:71::");
 runtime.Advertise(&up,true,false);
 assert(runtime.daemon_->params.dnses_.empty() && runtime.daemon_->params.rdnssLifetime_==0);
 runtime.Advertise(&up,true,true);assert(runtime.daemon_->params.dnses_.size()==1);
 NearlinkIpv6Runtime collision;collision.ifindex_=7;collision.layer2_=runtime.layer2_;
 NetLinkInfo adjacent;adjacent.netAddrList_.push_back({AF_INET6,64,"2001:db8:10:20::1"});
 adjacent.netAddrList_.push_back({AF_INET6,64,"2001:db8:10:21::1"});
 assert(!collision.Advertise(&adjacent,true));assert(collision.prefix_.empty());
 in6_addr ula{};std::string ulaText;NetLinkInfo privateUp;
 privateUp.netAddrList_.push_back({AF_INET6,64,"fd77:77:1::99"});
 assert(DeriveDownstreamPrefix(&privateUp,ula,ulaText)&&ulaText=="fd77:77:1:1::");
 NetLinkInfo cellular;cellular.ifaceName_="rmnet0";
 {std::ofstream f("if_inet6_upstream.txt");
  f<<"240e04041a01463235590ac22965dfec 0a 40 00 80 rmnet0\n";
  f<<"20010db8009800010000000000000001 0a 40 00 20 rmnet0\n";
  f<<"20010db8009900010000000000000001 0b 40 00 80 wlan0\n";}
 in6_addr recovered{};std::string recoveredText;
 assert(DeriveDownstreamPrefix(&cellular,recovered,recoveredText));
 assert(recoveredText=="240e:404:1a01:4633::");
 NearlinkIpv6Runtime missingRoute;missingRoute.ifindex_=7;missingRoute.layer2_="02:11:22:33:44:55";
 routedInterface="rmnet0";
 missingRoute.Advertise(&cellular,true);
 assert(missingRoute.daemon_->params.routerLifetime_==180);
 NetLinkInfo wifi=cellular;wifi.ifaceName_="wlan0";
 missingRoute.Advertise(&wifi,true);
 assert(missingRoute.daemon_->params.routerLifetime_==0); // Another connected interface is not the selected upstream.
 routedInterface="wlan0";
 missingRoute.Advertise(&wifi,true);
 assert(missingRoute.daemon_->params.routerLifetime_==180);
 routedInterface.clear();
 missingRoute.Advertise(&cellular,true);
 assert(missingRoute.daemon_->params.routerLifetime_==0);
 puts("gateway: automatic PAN-style prefix, L2 EUI-64, automatic RDNSS, collision guard, renumber/withdrawal PASS");
}
''')
    exe = out / 'test.exe'
    subprocess.run(['g++', '-std=c++17', f'-I{out}', str(out / 'test.cpp'), '-o', str(exe), '-lws2_32'], check=True)
    subprocess.run([str(exe)], cwd=out, check=True)
