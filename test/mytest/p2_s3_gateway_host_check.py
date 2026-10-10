"""Execute production prefix/RA reconciliation; substitute syscalls, Netsys and RA socket I/O."""
from pathlib import Path
import subprocess
import tempfile
import os

repo = Path(__file__).resolve().parents[2]
src = repo / 'services/networksharemanager'
production = (src / 'src/nearlink_ipv6_runtime.cpp').read_text()
before_ref = os.environ.get('P3_SWITCH_BEFORE_REF')
if before_ref:
    production = subprocess.check_output(['git', '-c', f'safe.directory={repo.as_posix()}', '-C', str(repo),
        'show', before_ref + ':services/networksharemanager/src/nearlink_ipv6_runtime.cpp']).decode()
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
inline static int starts=0;inline static int failNext=0;inline static std::vector<RaParams> sent;RaParams params;
std::string iface;int Init(const char*s){iface=s;return 0;}int StartRa(){++starts;return 0;}void StopRa(){}
void BuildNewRa(const RaParams&p){params=p;}bool AdvertiseNow(){if(failNext>0){--failNext;return false;}sent.push_back(params);return true;}
};
}
''')
    put('net_link_info.h', '''#pragma once
#include <string>
#include <vector>
namespace OHOS::NetManagerStandard {
struct Address {int family_=0;unsigned prefixlen_=0;std::string address_;};
struct Route {Address destination_;};struct NetLinkInfo {uint16_t mtu_=0;std::string ifaceName_;std::vector<Address> netAddrList_;std::vector<Route> routeList_;};
}
''')
    put('nearlink_ipv6_runtime.h', (src / 'include/nearlink_ipv6_runtime.h').read_text())
    put('nearlink_peer_address_pool.h', (src / 'include/nearlink_peer_address_pool.h').read_text())
    put('sysfs/rmnet0/mtu', '1400\n')
    advertise = advertise.replace('/proc/net/if_inet6', 'if_inet6.txt')
    advertise = advertise.replace('/sys/class/net/', 'sysfs/')
    put('test.cpp', r'''
#include <memory>
#include <set>
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
#include "nearlink_peer_address_pool.h"
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
int routeQueries=0;
bool HasIpv6DefaultRouteOnInterface(const std::string &iface)
{
    ++routeQueries;
    return iface == routedInterface;
}
''' + advertise + r'''
}
using namespace OHOS::NetManagerStandard;
void kernelAddress(const std::string &address, unsigned flags, const std::string &iface="sleip0")
{
 in6_addr binary{};assert(inet_pton(AF_INET6,address.c_str(),&binary)==1);
 std::ofstream f("if_inet6.txt");
 for(unsigned char byte:binary.s6_addr){char hex[3]{};snprintf(hex,sizeof(hex),"%02x",byte);f<<hex;}
 f<<" 07 40 00 "<<std::hex<<flags<<" "<<iface<<"\n";
}
int main(){
 NearlinkIpv6Runtime runtime;runtime.ifindex_=7;runtime.layer2_="02:11:22:33:44:55";
 assert(!runtime.Advertise(nullptr,false));
 NetLinkInfo linkLocal;linkLocal.ifaceName_="rmnet0";
 linkLocal.netAddrList_.push_back({AF_INET6,64,"fe80::1234"});
 {std::ofstream f("if_inet6_upstream.txt");}
 assert(!runtime.Advertise(&linkLocal,true));assert(routeQueries==0);
 assert(runtime.prefix_.empty() && !runtime.HasDefaultRouter());
 NetLinkInfo up;up.netAddrList_.push_back({AF_INET6,64,"2001:db8:10:20::1234"});
 assert(NearlinkIpv6Runtime::DeriveGatewayPrefix(&up,0)=="2001:db8:10:21::");
 assert(NearlinkIpv6Runtime::DeriveGatewayPrefix(&up,1)=="2001:db8:10:22::");
 std::set<std::string> derived;
 for(unsigned slot=0;slot<32;++slot) assert(derived.insert(NearlinkIpv6Runtime::DeriveGatewayPrefix(&up,slot)).second);
 up.netAddrList_[0].address_="2001:db8:10:ff::1";
 assert(NearlinkIpv6Runtime::DeriveGatewayPrefix(&up,0)=="2001:db8:10:100::");
 up.netAddrList_[0].address_="2001:db8:10:20::1234";
 NetLinkInfo colliding=up;colliding.netAddrList_.push_back({AF_INET6,64,"2001:db8:10:22::1"});
 assert(!NearlinkIpv6Runtime::DeriveGatewayPrefix(&colliding,0).empty());
 assert(NearlinkIpv6Runtime::DeriveGatewayPrefix(&colliding,1).empty());
 assert(NearlinkIpv6Runtime::DeriveGatewayPrefix(&up,32).empty());

 Route def;def.destination_.family_=AF_INET6;up.routeList_.push_back(def);
 {std::ofstream f("if_inet6.txt");}
 assert(!runtime.Advertise(&up,true));assert(RouterAdvertisementDaemon::starts==0);
 assert(runtime.prefix_=="2001:db8:10:21::");
 assert(runtime.gateway_=="2001:db8:10:21:11:22ff:fe33:4455");
 assert(runtime.addresses_.size()==1);
 kernelAddress(runtime.gateway_,0x40); // Tentative address must not be advertised as DNS or a prefix.
 assert(!runtime.Advertise(&up,true));assert(RouterAdvertisementDaemon::starts==0);
 kernelAddress(runtime.gateway_,0);
 RouterAdvertisementDaemon::failNext=1;
 assert(!runtime.Advertise(&up,true));assert(runtime.advertisedPrefix_.empty());
 assert(runtime.dns_.empty()); // A failed first RA cannot publish success in the controller.
 assert(runtime.Advertise(&up,true));assert(RouterAdvertisementDaemon::starts==1);
 assert(runtime.daemon_->params.dnses_.size()==1);
 assert(runtime.daemon_->params.prefixes_.size()==1);
 // A narrower upstream MTU must be sent immediately, including an unchanged prefix/DNS.
 up.mtu_=1400;
 auto beforeMtu=RouterAdvertisementDaemon::sent.size();
 assert(runtime.Advertise(&up,true));
 assert(runtime.daemon_->params.mtu_==1400 && RouterAdvertisementDaemon::sent.size()==beforeMtu+1);
 up.mtu_=1280;assert(runtime.Advertise(&up,true));assert(runtime.daemon_->params.mtu_==1280);
 up.mtu_=9000;assert(runtime.Advertise(&up,true));assert(runtime.daemon_->params.mtu_==1500);
 up.mtu_=0;assert(runtime.Advertise(&up,true));assert(runtime.daemon_->params.mtu_==1500);
 assert(runtime.daemon_->params.routerLifetime_==180);
 RouterAdvertisementDaemon::failNext=1;
 assert(!runtime.Advertise(&up,true,false));assert(!runtime.dns_.empty());
 assert(runtime.Advertise(&up,true,false));assert(runtime.dns_.empty());
 RouterAdvertisementDaemon::failNext=1;
 assert(!runtime.Advertise(&up,true,true));assert(runtime.dns_.empty());
 assert(runtime.Advertise(&up,true,true));assert(!runtime.dns_.empty());
 assert(runtime.Advertise(&up,true));assert(RouterAdvertisementDaemon::starts==1);
 up.netAddrList_.clear();up.netAddrList_.push_back({AF_INET6,64,"2001:db8:10:30::abcd"});
 assert(!runtime.Advertise(&up,true));assert(runtime.retired_.size()==1);
 assert(runtime.daemon_->params.prefixes_.size()==1);
 assert(runtime.daemon_->params.prefixes_[0].preferredLifetime==0);
 assert(runtime.addresses_.size()==2); // old address survives while new DAD is pending
 assert(runtime.OwnsPrefix("2001:db8:10:21::") && runtime.OwnsPrefix("2001:db8:10:31::"));
 kernelAddress(runtime.gateway_,0x08); // DAD failure must not publish an active prefix.
 assert(!runtime.Advertise(&up,true));assert(runtime.daemon_->params.dnses_.empty());
 kernelAddress(runtime.gateway_,0);
 assert(runtime.Advertise(&up,true));assert(runtime.addresses_.size()==2);
 runtime.retired_[0].until=std::chrono::steady_clock::now()-std::chrono::seconds(1);
 assert(runtime.Advertise(&up,true));assert(runtime.retired_.empty());assert(runtime.addresses_.size()==1);
 // Reserve the active slot before retirement. A full explicit routed pool waits
 // for cleanup without orphaning the last active address/route on every retry.
 for(const char* address : {"2001:db8:10:40::1","2001:db8:10:50::1","2001:db8:10:60::1","2001:db8:10:70::1"}) {
  up.netAddrList_[0].address_=address;runtime.Advertise(&up,true);
 }
 assert(runtime.retired_.size()==3 && runtime.prefix_=="2001:db8:10:61::");
 assert(runtime.addresses_.size()==4 && runtime.routeOwned_);
 assert(runtime.daemon_->params.routerLifetime_==0 && runtime.daemon_->params.dnses_.empty());
 auto deadlines=runtime.retired_;
 for(int retry=0;retry<10;++retry) {
  assert(!runtime.Advertise(&up,true));assert(runtime.prefix_=="2001:db8:10:61::");
  for(size_t i=0;i<deadlines.size();++i)assert(runtime.retired_[i].until==deadlines[i].until);
 }
 for(auto &old:runtime.retired_)old.until=std::chrono::steady_clock::now()-std::chrono::seconds(1);
 NetsysController::failRemove=true;runtime.Advertise(&up,true);
 assert(!runtime.retired_.empty()); // failed resources are retained for cleanup, never silently dropped
 NetsysController::failRemove=false;runtime.Advertise(&up,true);
 assert(runtime.retired_.size()==1 && runtime.prefix_=="2001:db8:10:71::");
 kernelAddress(runtime.gateway_,0);
 runtime.Advertise(&up,true,false);
 assert(runtime.daemon_->params.dnses_.empty() && runtime.daemon_->params.rdnssLifetime_==0);
 runtime.Advertise(&up,true,true);assert(runtime.daemon_->params.dnses_.size()==1);
 // Controller keeps the logical prefix while the upstream is absent. Exercise
 // actual RA/DNS/default withdrawal and restoration without another address.
 runtime.SetGatewayPrefix(runtime.CurrentPrefix());
 auto active=runtime.CurrentPrefix();auto addressCount=runtime.addresses_.size();auto retiredCount=runtime.retired_.size();
 for(int cycle=0;cycle<20;++cycle) {
  assert(runtime.Advertise(nullptr,false));
  assert(runtime.daemon_->params.routerLifetime_==0 && runtime.CurrentPrefix()==active);
  up.netAddrList_[0].address_="2001:db8:"+std::to_string(cycle+100)+":20::1";
  assert(runtime.Advertise(&up,true));assert(runtime.HasDefaultRouter());
  assert(runtime.addresses_.size()==addressCount && runtime.retired_.size()==retiredCount);
 }
 puts("P3-S3 switch regression: 20 RA off/on cycles, bounded admission, failed cleanup retry PASS");
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
 kernelAddress(missingRoute.gateway_,0);
 missingRoute.Advertise(&cellular,true);
 assert(missingRoute.daemon_->params.routerLifetime_==180);
 assert(missingRoute.daemon_->params.mtu_==1400); // Recover omitted cellular link MTU from selected sysfs link.
 NetLinkInfo wifi=cellular;wifi.ifaceName_="wlan0";
 missingRoute.Advertise(&wifi,true);
 assert(missingRoute.daemon_->params.routerLifetime_==0); // Another connected interface is not the selected upstream.
 routedInterface="wlan0";
 missingRoute.Advertise(&wifi,true);
 kernelAddress(missingRoute.gateway_,0);
 missingRoute.Advertise(&wifi,true);
 assert(missingRoute.daemon_->params.routerLifetime_==180);
 assert(missingRoute.daemon_->params.mtu_==1500); // Do not reuse rmnet0 MTU for the selected Wi-Fi link.
 routedInterface.clear();
 missingRoute.Advertise(&cellular,true);
 assert(missingRoute.daemon_->params.routerLifetime_==0);
 // Product pools scale beyond three seats, reject overlap/insufficient and malformed pools.
 for (unsigned capacity : {1u,2u,3u,5u,7u,32u}) {
  std::set<std::string> subnets,prefixes;
  for(unsigned slot=0;slot<capacity;++slot) {
   NearlinkPeerAddresses address;
   assert(NearlinkPeerAddresses::Allocate("172.24.0.0/16","fd77:6e6c:6970::/48",capacity,slot,address,true));
   assert(subnets.insert(address.subnet).second && prefixes.insert(address.prefix).second);
   assert(address.Conflicts(address.gateway,24) && !address.Conflicts("192.168.62.1",24));
  }
 }
 NearlinkPeerAddresses address;
 assert(!NearlinkPeerAddresses::Allocate("172.24.0.1/16","fd77::/48",2,0,address,true));
 assert(!NearlinkPeerAddresses::Allocate("172.24.0.0/24","fd77::/64",2,0,address,true));
 assert(!NearlinkPeerAddresses::Allocate("172.24.0.0/16","fd77::/64",2,0,address,true));
 assert(!NearlinkPeerAddresses::Allocate("8.0.0.0/8","fd77::/48",2,0,address,true));
 NearlinkIpv6Runtime a,b;
 a.iface_="sleip0";a.ifindex_=7;a.layer2_="02:11:22:33:44:55";a.configuredPrefix_="fd77:6e6c:6970::";
 b.iface_="sleip1";b.ifindex_=7;b.layer2_=a.layer2_;b.configuredPrefix_="fd77:6e6c:6970:1::";
 {std::ofstream f("if_inet6.txt");}
 assert(!a.Advertise(nullptr,false) && !b.Advertise(nullptr,false));
 kernelAddress(a.gateway_,0);assert(a.Advertise(nullptr,false));
 assert(!b.Advertise(nullptr,false)); // slot zero's DAD evidence cannot validate slot one
 kernelAddress(b.gateway_,0x08,"sleip1");assert(!b.Advertise(nullptr,false));
 kernelAddress(b.gateway_,0,"sleip1");assert(b.Advertise(nullptr,false));
 assert(a.daemon_->iface=="sleip0" && b.daemon_->iface=="sleip1");
 assert(a.prefix_!=b.prefix_ && a.gateway_!=b.gateway_);
 assert(a.daemon_->params.routerLifetime_==0 && b.daemon_->params.routerLifetime_==0);
 assert(a.daemon_->params.rdnssLifetime_==180 && b.daemon_->params.rdnssLifetime_==180);
 assert(a.daemon_->params.prefixes_.size()==1 && b.daemon_->params.prefixes_.size()==1);
 puts("gateway: automatic PAN-style prefix, L2 EUI-64, automatic RDNSS, collision guard, renumber/withdrawal PASS");
}
''')
    exe = out / 'test.exe'
    subprocess.run(['g++', '-std=c++17', f'-I{out}', str(out / 'test.cpp'), '-o', str(exe), '-lws2_32'], check=True)
    subprocess.run([str(exe)], cwd=out, check=True)
