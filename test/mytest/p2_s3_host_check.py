"""Compile the production controller and family aggregation against explicit platform doubles.
Does not prove Binder, kernel, product compilation, radio, routing, or DNS reachability.
"""
from pathlib import Path
import subprocess
import tempfile
import os

repo = Path(__file__).resolve().parents[2]
workspace = repo.parent
source = repo / 'services/networksharemanager'
with tempfile.TemporaryDirectory(prefix='p2-s3-') as directory:
    out = Path(directory)
    def put(name, text):
        file = out / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(text, encoding='utf-8')
    put('mock.h', (Path(__file__).with_name('p2_s3_platform_mock.h')).read_text())
    put('parcel.h', '''#pragma once
#include <string>
#include <cstdint>
class Parcel { public:
bool WriteInt32(int32_t){return true;} bool WriteUint32(uint32_t){return true;}
bool WriteUint64(uint64_t){return true;} bool WriteBool(bool){return true;}
bool WriteString(const std::string&){return true;}
bool ReadInt32(int32_t&){return false;} bool ReadUint32(uint32_t&){return false;}
bool ReadUint64(uint64_t&){return false;} bool ReadBool(bool&){return false;}
bool ReadString(std::string&){return false;}
};
class Parcelable { public: virtual ~Parcelable()=default; virtual bool Marshalling(Parcel&) const=0; };
''')
    for name in ['nearlink_ipshare_client.h', 'networkshare_configuration.h', 'net_link_info.h',
                 'net_conn_client.h', 'net_conn_callback_stub.h', 'nearlink_host.h', 'net_manager_constants.h',
                 'netmgr_ext_log_wrapper.h', 'netsys_controller.h', 'networkshare_tracker.h', 'net_manager_ext_constants.h', 'securec.h']:
        put(name, '#pragma once\n#include "mock.h"\n')
    # Socket parsing uses WinSock, not a home-made IPv6 parser.
    put('netinet/ip.h', '#pragma once\n#include <winsock2.h>\n#include <ws2tcpip.h>\n#undef ERROR\n#ifndef InetPtonA\nextern "C" int inet_pton(int,const char*,void*);\n#endif\n#ifndef InetNtopA\nextern "C" const char* inet_ntop(int,const void*,char*,size_t);\n#endif\n')
    put('arpa/inet.h', '#include <netinet/ip.h>\n')
    put('net/if.h', '#pragma once\ninline unsigned mockIfIndex=7;\ninline unsigned if_nametoindex(const char*) { return mockIfIndex; }\n')
    put('nearlink_ipv6_runtime.h', '''#pragma once
#include "mock.h"
namespace OHOS::NetManagerStandard {
class NearlinkIpv6Runtime { public:
bool Prepare(bool,const std::string&,const std::string& = "sleip0",const std::string& = ""){return call("ipv6-prepare")==0;}
std::string prefix;bool routed=false;
inline static unsigned upstreamEpoch=0;
bool HasPrefix()const{return !prefix.empty();}
const std::string &CurrentPrefix()const{return prefix;}
void SetGatewayPrefix(const std::string &p){prefix=p;}
bool OwnsPrefix(const std::string &p)const{return !p.empty()&&prefix==p;}
bool HasDefaultRouter()const{return routed;}
static std::string DeriveGatewayPrefix(const NetLinkInfo *u,uint32_t s){return u ? "2001:db8:"+std::to_string(10+upstreamEpoch)+":"+std::to_string(s+1)+"::" : "";}
bool Advertise(const NetLinkInfo*,bool f,bool){routed=f;return call("ra")==0;}
bool Cleanup(){return call("ipv6-cleanup")==0;}
}; }
''')
    # Copy only the controller header so the platform runtime double is selected explicitly.
    put('nearlink_ipshare_controller.h', (source / 'include/nearlink_ipshare_controller.h').read_text())
    put('nearlink_ipshare_controller.cpp', (source / 'src/nearlink_ipshare_controller.cpp').read_text())
    put('nearlink_peer_address.cpp', (source / 'src/nearlink_peer_address.cpp').read_text())
    before_ref = os.environ.get('P3_SWITCH_BEFORE_REF')
    upstream_source = (source / 'src/nearlink_peer_upstream.cpp').read_text()
    if before_ref:
        upstream_source = subprocess.check_output(['git', '-c', f'safe.directory={repo.as_posix()}',
            '-C', str(repo), 'show', before_ref + ':services/networksharemanager/src/nearlink_peer_upstream.cpp']).decode()
    put('nearlink_peer_upstream.cpp', upstream_source)
    put('nearlink_family_validation.h', '#pragma once\nnamespace OHOS::NetManagerStandard { struct NearlinkFamilyValidation { int ipv4=0,ipv6=0; }; inline NearlinkFamilyValidation ValidateNearlinkFamilies(int,bool,bool) {return {};} }\n')
    put('dhcp_c_api.h', '''#pragma once
#include "mock.h"
#include "dhcp_result_event.h"
#include "dhcp_l3_ipv6.h"
inline int RegisterDhcpClientCallBack(const char*,const ClientCallBack*) { return call("dhcp-callback"); }
inline int RegisterDhcpClientL3Session(const char*,uint64_t,
 void (*)(uint64_t,int,const char*,const DhcpResult*,const DhcpL3Ipv6Snapshot*),
 void (*)(uint64_t,int,const char*,const char*)) {return 0;}
inline int RegisterDhcpClientL3Ipv6CallBack(const char*,void (*)(const char*,const DhcpL3Ipv6Snapshot*)) {return 0;}
inline int StartDhcpClientL3(const RouterConfig*,const uint8_t*,size_t) {return call("dhcp-start");}
inline int StopDhcpClient(const char*,bool,bool) {return call("dhcp-stop");}
inline int SetDhcpRange(const char*,const DhcpRange*) {return call("range");}
inline int StartDhcpServer(const char*) {return call("server-start");}
inline int StopDhcpServer(const char*) {return call("server-stop");}
''')
    put('test.cpp', r'''
#include "mock.h"
#include <chrono>
#include "dhcp_c_api.h"
#include "nearlink_family_network.h"
#include "nearlink_ipv6_runtime.h"
#define private public
#include "nearlink_ipshare_controller.h"
#undef private
#include "nearlink_ipshare_controller.cpp"
#include "nearlink_peer_address.cpp"
#include "nearlink_peer_upstream.cpp"
using namespace OHOS::NetManagerStandard;
int main() {
    auto c=NearlinkIpShareController::GetInstance();
    auto &q=NetworkShareTracker::GetInstance();
    auto &net=NetConnClient::GetInstance();
    const std::string peer="02:11:22:33:44:66";
    assert(c->StartTerminal(peer,3)==0); q.Drain();
    OHOS::Nearlink::NearlinkIpShareStatus link;
    link.role=NearlinkIpShareRole::TERMINAL; link.state=NearlinkIpShareState::CHANNEL_READY;
    link.peerAddress=peer; link.ifaceName="sleip0"; link.generation=1; link.sequence=1;
    link.selectedMode=3;
    OHOS::Nearlink::NearlinkIpShareClient::GetInstance().snapshot=link;
    c->OnNearlinkStatus(link); q.Drain();
    DhcpL3Ipv6Snapshot addresses{}; addresses.addressCount=1;
    auto &a=addresses.addresses[0]; strcpy(a.address,"fd77:77:1::2");
    a.ifindex=7; a.prefixLength=64; a.preferredLifetime=120; a.validLifetime=300;
    c->OnIpv6Addresses("sleip0",addresses); q.Drain();
    DhcpResult v6{}; v6.iptype=1; strcpy(v6.strOptRouter1,"fe80::1");
    v6.ipv6LifeTime.routerLifeTime=180; v6.dnsList.dnsNumber=1;
    strcpy(v6.dnsList.dnsAddr[0],"fd77:77:1::53");
    c->OnDhcpSuccess(0,"sleip0",v6); q.Drain();
    assert(net.available && net.lastLink.netAddrList_.size()==1);
    assert(c->status_.ipv6.configurationAvailable && !c->status_.ipv6.externalAvailable);
    { Parcel parcel; assert(c->status_.Marshalling(parcel)); }
    auto supplier=c->netSupplierId_;
    auto session=c->generation_.load();
    c->OnDhcpFailure(0x10001,"sleip0","dhcpv6 failed",session); q.Drain();
    assert(c->status_.ipv6.configurationAvailable);
    c->OnDhcpFailure(1,"sleip0","late old session",session+123); q.Drain();
    assert(c->netSupplierId_==supplier && c->status_.ipv6.configurationAvailable);
    DhcpResult v4{}; v4.iptype=0; v4.isOptSuc=true; v4.uOptLeasetime=600;
    strcpy(v4.strOptClientId,"192.168.77.2"); strcpy(v4.strOptSubnet,"255.255.255.0");
    strcpy(v4.strOptRouter1,"192.168.77.1"); strcpy(v4.strOptDns1,"192.168.77.1");
    c->OnDhcpSuccess(0,"sleip0",v4); q.Drain();
    assert(net.lastLink.netAddrList_.size()==2 && net.lastLink.routeList_.size()==4);
    assert(c->netSupplierId_==supplier && net.registrations==1);
    { Parcel parcel; assert(c->status_.Marshalling(parcel)); }
    c->OnDhcpFailure(4,"sleip0","renew"); q.Drain();
    assert(net.lastLink.netAddrList_.size()==2);
    c->OnDhcpFailure(1,"sleip0","lost"); q.Drain();
    assert(net.lastLink.netAddrList_.size()==1 && net.lastLink.netAddrList_[0].family_==AF_INET6);
    assert(c->netSupplierId_==supplier && c->nearlinkStarted_);
    c->OnDhcpSuccess(0,"sleip0",v4); q.Drain();
    v6.dnsList.dnsNumber=0; c->OnDhcpSuccess(0,"sleip0",v6); q.Drain();
    assert(net.lastLink.dnsList_.size()==1 && c->status_.ipv4.configurationAvailable);
    assert(!c->status_.ipv6.configurationAvailable);
    addresses.addressCount=0; c->OnIpv6Addresses("sleip0",addresses);
    c->OnDhcpSuccess(0,"sleip0",v6); q.Drain();
    assert(net.lastLink.netAddrList_.size()==1 && net.lastLink.netAddrList_[0].family_==AF_INET);
    const auto &e=OHOS::Nearlink::NearlinkIpShareClient::GetInstance().evidence;
    assert(e.size()==2 && e.back().validLifetime==0);
    // A failed family update restores the committed aggregate without deleting the other family.
    errors["link-info"]=-7; strcpy(v4.strOptClientId,"192.168.77.3");
    c->OnDhcpSuccess(0,"sleip0",v4); q.Drain();
    assert(net.lastLink.netAddrList_[0].address_=="192.168.77.2" && c->netSupplierId_==supplier);
    auto expiry=c->pendingLeaseExpiry_;
    errors.clear();
    auto starts=std::count(calls.begin(),calls.end(),"dhcp-start");
    c->RetryTerminalNetwork();
    assert(!c->ipv4PublishPending_ && c->leaseExpiry_==expiry);
    assert(std::count(calls.begin(),calls.end(),"dhcp-start")==starts);
    assert(net.lastLink.netAddrList_[0].address_=="192.168.77.3");
    c->OnDhcpSuccess(0,"sleip0",v4); c->StopTerminal(); q.Drain();
    assert(c->status_.state==NearlinkIpShareState::IDLE && !c->nearlinkStarted_);
    c->OnDhcpSuccess(0,"sleip0",v4); q.Drain();
    assert(c->netSupplierId_==0);
    errors["link-info"]=-7;
    assert(c->StartTerminal(peer,3)==0);q.Drain();
    link.generation=2;link.sequence=1;OHOS::Nearlink::NearlinkIpShareClient::GetInstance().snapshot=link;
    c->OnNearlinkStatus(link);q.Drain();c->OnDhcpSuccess(0,"sleip0",v4);q.Drain();
    assert(c->ipv4PublishPending_ && !c->retryIpv4_ && c->netSupplierId_==0);
    expiry=c->pendingLeaseExpiry_;errors.clear();c->RetryTerminalNetwork();
    assert(c->status_.ipv4.configurationAvailable && c->leaseExpiry_==expiry);
    errors["link-info"]=-7;c->OnDhcpSuccess(0,"sleip0",v4);q.Drain();
    // Force a different address so the normal equality fast path is not used.
    strcpy(v4.strOptClientId,"192.168.77.4");c->OnDhcpSuccess(0,"sleip0",v4);q.Drain();
    c->pendingLeaseExpiry_=std::chrono::steady_clock::now()-std::chrono::seconds(1);
    errors.clear();c->RetryTerminalNetwork();assert(!c->ipv4PublishPending_&&c->retryIpv4_);
    c->StopTerminal();q.Drain();
    errors["address-add"]=-7;c->ConfigureGateway();
    assert(c->dnsProxyStarted_ && !c->dhcpServerStarted_ && c->status_.ipv4.hasError);
    net.hasDefault=true;c->dualStack_=true;c->channelReady_=true;
    errors.clear();errors["dns-set"]=-7;c->ConfigureUpstream();
    assert(c->status_.ipv6.hasError && c->status_.ipv6.error.stage=="DNS");
    assert(!c->status_.ipv6.configurationAvailable);
    errors.clear();errors["ra"]=-7;c->ConfigureUpstream();
    assert(c->status_.ipv6.phase==1 && !c->status_.ipv6.hasError && !c->status_.ipv6.configurationAvailable);
    c->ipv6GatewayPendingSince_-=std::chrono::seconds(11);c->ConfigureUpstream();
    assert(c->status_.ipv6.phase==3 && c->status_.ipv6.hasError && c->status_.ipv6.error.stage=="PREFIX");
    errors.clear();c->ConfigureUpstream();assert(c->status_.ipv6.configurationAvailable);
    assert(c->status_.ipv6.phase==2 && !c->status_.ipv6.hasError && c->status_.ipv6.error.code==0);
    c->Cleanup();net.hasDefault=false;
    auto oldSession=session;
    errors["ipv6-prepare"]=-1;
    assert(c->StartTerminal(peer,3)==0);q.Drain();
    link.generation=3;link.sequence=1;OHOS::Nearlink::NearlinkIpShareClient::GetInstance().snapshot=link;
    c->OnNearlinkStatus(link);q.Drain();
    c->OnDhcpSuccess(0,"sleip0",v4,oldSession);q.Drain();assert(c->netSupplierId_==0);
    c->OnDhcpSuccess(0,"sleip0",v4,c->generation_);q.Drain();
    assert(c->status_.ipv4.configurationAvailable && c->nearlinkStarted_);
    errors.clear();errors["ipv6-cleanup"]=-1;c->StopTerminal();q.Drain();
    assert(c->status_.state==NearlinkIpShareState::ERROR && c->status_.errorStage=="CLEANUP");
    errors.clear();c->StopTerminal();q.Drain();assert(c->status_.state==NearlinkIpShareState::IDLE);
    int32_t supported=0;assert(c->GetSupportedMaxTerminals(supported)==0 && supported==2);
    assert(c->StartGatewayAny(1,3)!=0);
    q.delayed.clear(); // Discard previous terminal sessions' maintenance ticks.
    c->configuration_.pool4="172.24.0.0/24";
    assert(c->StartGatewayAny(3,2)!=0); // preflight pool insufficient for capacity
    c->configuration_.pool4="172.24.0.0/16";
    assert(c->StartGatewayAny(3,2)==0);q.Drain();
    link=OHOS::Nearlink::NearlinkIpShareStatus{};
    link.role=NearlinkIpShareRole::GATEWAY;link.state=NearlinkIpShareState::SERVING_NO_UPSTREAM;
    link.generation=4;link.sequence=1;link.serviceReady=true;
    OHOS::Nearlink::NearlinkIpShareClient::GetInstance().snapshot=link;
    c->OnNearlinkStatus(link);q.Drain();
    assert(c->status_.state==NearlinkIpShareState::SERVING_NO_UPSTREAM && c->addressPeers_.empty());
    link.peerLinks={{0,10,3,"sleip0"},{1,11,1,"sleip1"}};
    link.state=NearlinkIpShareState::CHANNEL_READY;link.sequence=2;
    OHOS::Nearlink::NearlinkIpShareClient::GetInstance().snapshot=link;
    c->OnNearlinkStatus(link);q.Drain();
    assert(c->addressPeers_.size()==2);
    assert(c->addressPeers_[0].dhcpStarted && c->addressPeers_[1].dhcpStarted);
    assert(c->addressPeers_[0].addresses.gateway=="172.24.0.1");
    assert(c->addressPeers_[1].addresses.gateway=="172.24.1.1");
    assert(c->addressPeers_[0].ipv6Ready && !c->addressPeers_[1].ipv6Ready);
    // Per-family failure on slot one leaves the first slot's lease and RA intact.
    c->addressPeers_[1].dhcpStarted=false;errors["server-start"]=-7;
    c->ReconcileGatewayPeers(link);
    assert(c->addressPeers_[0].dhcpStarted && c->addressPeers_[0].ipv6Ready && !c->addressPeers_[1].dhcpStarted);
    errors.clear();c->ReconcileGatewayPeers(link);assert(c->addressPeers_[1].dhcpStarted);
    c->addressPeers_[1].dhcpStarted=false;errors["server-start"]=-7;errors["server-stop"]=-8;
    c->ReconcileGatewayPeers(link);
    assert(c->addressPeers_[1].dhcpStarted && c->addressPeers_[1].ipv4Error==-7);
    link.peerLinks[1].generation=99;c->ReconcileGatewayPeers(link);
    assert(c->addressPeers_[1].generation==11 && c->addressPeers_[0].dhcpStarted);
    link.peerLinks[1].generation=11;errors.clear();c->ReconcileGatewayPeers(link);
    assert(c->addressPeers_[1].dhcpStarted && c->addressPeers_[1].ipv4Error==0);
    // Failed cleanup retains old ownership and blocks reuse of the slot by a new epoch.
    link.peerLinks[1].generation=12;errors["server-stop"]=-7;
    c->ReconcileGatewayPeers(link);assert(c->addressPeers_[1].generation==11);
    errors.clear();c->ReconcileGatewayPeers(link);assert(c->addressPeers_[1].generation==12);
    NetLinkInfo conflict;INetAddr conflictAddress;conflictAddress.family_=AF_INET;
    conflictAddress.address_="172.24.1.9";conflictAddress.prefixlen_=24;conflict.netAddrList_.push_back(conflictAddress);
    assert(c->ConfigureGatewayPeerIpv4(c->addressPeers_[1],&conflict)!=0);
    assert(!c->addressPeers_[1].dhcpStarted && c->addressPeers_[0].dhcpStarted && c->addressPeers_[0].ipv6Ready);
    assert(c->ConfigureGatewayPeerIpv4(c->addressPeers_[1],nullptr)==0);
    errors["ra"]=-1;c->ReconcileGatewayPeers(link);
    c->addressPeers_[0].ipv6PendingSince-=std::chrono::seconds(11);c->ReconcileGatewayPeers(link);
    assert(c->addressPeers_[0].ipv6Error!=0 && c->addressPeers_[0].dhcpStarted && c->addressPeers_[1].dhcpStarted);
    errors.clear();c->ReconcileGatewayPeers(link);assert(c->addressPeers_[0].ipv6Error==0);
    link.peerLinks[1].releasing=true;errors["server-stop"]=-7;
    calls.clear();c->ReconcileGatewayPeers(link);
    assert(c->addressPeers_.count(1)==1 && std::find(calls.begin(),calls.end(),"peer-release:12")==calls.end());
    errors.clear();calls.clear();c->ReconcileGatewayPeers(link);
    assert(c->addressPeers_.count(1)==0 && std::find(calls.begin(),calls.end(),"peer-release:12")!=calls.end());
    link.peerLinks[1].generation=13;link.peerLinks[1].releasing=false;c->ReconcileGatewayPeers(link);
    assert(c->addressPeers_[1].generation==13 && c->addressPeers_[1].dhcpStarted);
    // Removing slot zero must preserve slot one, including its DHCP instance.
    link.peerLinks.erase(link.peerLinks.begin());link.sequence=3;
    OHOS::Nearlink::NearlinkIpShareClient::GetInstance().snapshot=link;
    c->OnNearlinkStatus(link);q.Drain();
    assert(c->addressPeers_.size()==1 && c->addressPeers_[1].dhcpStarted);
    assert(c->status_.ifaceName=="sleip1" && c->status_.ipv4Address=="172.24.1.1");
    link.peerLinks.clear();link.sequence=4;
    OHOS::Nearlink::NearlinkIpShareClient::GetInstance().snapshot=link;
    c->OnNearlinkStatus(link);q.Drain();
    assert(c->addressPeers_.empty() && c->status_.ipv4Address.empty());
    assert(c->status_.state==NearlinkIpShareState::SERVING_NO_UPSTREAM);
    assert(!q.delayed.empty());auto tick=q.delayed.front();q.delayed.pop_front();tick();q.Drain();
    assert(c->addressPeers_.empty()); // queued maintenance cannot revive a removed peer
    assert(c->StartGatewayAny(3,2)==0 && c->StartGatewayAny(1,1)!=0);
    assert(c->StopGateway()==0);q.Drain();assert(c->status_.state==NearlinkIpShareState::IDLE);
    auto activeGateway=[&]() {
        q.delayed.clear();errors.clear();
        assert(c->StartGatewayAny(3,2)==0);q.Drain();
        link.role=NearlinkIpShareRole::GATEWAY;link.state=NearlinkIpShareState::CHANNEL_READY;
        ++link.generation;link.sequence=1;link.serviceReady=true;
        link.peerLinks={{0,20+link.generation*2,3,"sleip0"},{1,21+link.generation*2,3,"sleip1"}};
        OHOS::Nearlink::NearlinkIpShareClient::GetInstance().snapshot=link;
        c->OnNearlinkStatus(link);q.Drain();q.delayed.clear();
        assert(c->addressPeers_.size()==2 && c->gatewayReserved_);
    };
    auto retryStop=[&]() {assert(!q.delayed.empty());auto f=q.delayed.front();q.delayed.pop_front();f();q.Drain();};
    // P3-S3: two downstream ledgers, immediate upstream events, default loss, and independent retries.
    activeGateway();net.hasDefault=true;net.iface="wlan0";
    Route def4;def4.destination_.family_=AF_INET;net.upstreamProperties.routeList_={def4};
    c->OnUpstreamChanged();q.Drain();
    assert(c->addressPeers_[0].natEnabled && c->addressPeers_[1].natEnabled);
    assert(c->addressPeers_[0].interfaceForwarding && c->addressPeers_[1].interfaceForwarding);
    assert(c->status_.hasUpstream && c->status_.state==NearlinkIpShareState::SERVING);
    assert(c->addressPeers_[0].ipv6Routed && c->addressPeers_[1].ipv6Routed);
    assert(!c->status_.ipv4.externalAvailable && !c->status_.ipv6.externalAvailable);
    calls.clear();c->ConfigureUpstream();
    assert(std::find(calls.begin(),calls.end(),"nat-add")==calls.end());
    // DNS failure affects external readiness, preserves local DHCP and forwarding ownership.
    errors["dns-set"]=-7;c->OnUpstreamChanged();q.Drain();
    assert(!c->dnsUpstreamReady_ && c->addressPeers_[1].dhcpStarted && c->addressPeers_[1].natEnabled);
    errors.clear();c->OnUpstreamChanged();q.Drain();assert(c->dnsUpstreamReady_);
    // Failed old-upstream release blocks only its peer, never grafts its flags onto the new upstream.
    net.iface="rmnet0";net.netId=11;errors["nat-del:sleip1"]=-7;
    c->OnUpstreamChanged();q.Drain();
    assert(c->addressPeers_[0].upstreamIface=="rmnet0" && c->addressPeers_[0].natEnabled);
    assert(c->addressPeers_[1].upstreamIface=="wlan0" && c->addressPeers_[1].natEnabled);
    assert(!c->addressPeers_[1].interfaceForwarding && !c->addressPeers_[1].ipv6Routed);
    errors.clear();c->OnUpstreamChanged();q.Drain();
    assert(c->addressPeers_[1].upstreamIface=="rmnet0" && c->addressPeers_[1].natEnabled);
    // Losing only IPv4 default removes NAT but retains independently routed IPv6.
    net.upstreamProperties.routeList_.clear();c->ConfigureUpstream();
    assert(c->addressPeers_[0].natEnabled && c->addressPeers_[0].ipv6Routed);
    net.upstreamProperties.routeList_={def4};c->ConfigureUpstream();
    // Whole upstream loss retains both local families and G global listener/forwarding ownership.
    const auto prefix0=c->addressPeers_[0].ipv6.prefix;
    const auto prefix1=c->addressPeers_[1].ipv6.prefix;
    net.hasDefault=false;c->OnUpstreamChanged();q.Drain();
    assert(c->addressPeers_[0].ipv6.prefix==prefix0 && c->addressPeers_[1].ipv6.prefix==prefix1);
    assert(!c->status_.hasUpstream && !c->addressPeers_[0].natEnabled && !c->addressPeers_[1].natEnabled);
    assert(c->forwardingEnabled_ && c->dnsProxyStarted_ && c->addressPeers_[0].dhcpStarted);
    assert(c->status_.ipv6.hasError && !c->addressPeers_[0].ipv6Routed);
    net.hasDefault=true;errors["nat-add:sleip1"]=-7;c->ConfigureUpstream();
    assert(c->addressPeers_[0].natEnabled && !c->addressPeers_[1].natEnabled && c->addressPeers_[1].natAttempted);
    assert(c->addressPeers_[0].ipv6Routed && !c->addressPeers_[1].ipv6Routed);
    errors.clear();c->ConfigureUpstream();assert(c->addressPeers_[1].natEnabled);
    // Rapid off/on cycles must not mint ULA addresses or consume retirement slots.
    for(int cycle=0;cycle<20;++cycle) {
        net.hasDefault=false;c->ConfigureUpstream();
        assert(c->addressPeers_[0].ipv6.prefix==prefix0 && c->addressPeers_[1].ipv6.prefix==prefix1);
        ++NearlinkIpv6Runtime::upstreamEpoch;
        net.hasDefault=true;c->ConfigureUpstream();
        assert(c->addressPeers_[0].ipv6Routed && c->addressPeers_[1].ipv6Routed);
        assert(c->addressPeers_[0].ipv6.prefix==prefix0 && c->addressPeers_[1].ipv6.prefix==prefix1);
    }
    NearlinkIpv6Runtime::upstreamEpoch=0;
    // A new uplink candidate must not renumber either automatic NAT66 peer.
    c->addressPeers_[0].ipv6.prefix="2001:db8:aa:1::";
    c->addressPeers_[1].ipv6.prefix="2001:db8:aa:2::";
    c->ConfigureUpstream();
    assert(c->addressPeers_[0].ipv6.prefix=="2001:db8:aa:1::" && c->addressPeers_[0].ipv6Routed);
    assert(c->addressPeers_[1].ipv6.prefix=="2001:db8:aa:2::" && c->addressPeers_[1].ipv6Routed);
    c->ConfigureUpstream();
    // Explicit routed pool is bound to its upstream; a mismatch keeps local-only IPv6.
    c->configuration_.routedPool="2001:db8:100::/48";c->configuration_.routedUpstream="eth0";
    c->ConfigureUpstream();assert(!c->addressPeers_[0].ipv6Routed && c->status_.ipv6.hasError);
    c->configuration_.routedUpstream="rmnet0";c->ConfigureUpstream();assert(c->addressPeers_[1].ipv6Routed);
    c->configuration_.routedPool.clear();c->ConfigureUpstream();
    // A live/retiring prefix collision must not be advertised on a second peer.
    c->addressPeers_[0].ipv6.prefix.clear();
    c->addressPeers_[1].ipv6.SetGatewayPrefix("2001:db8:10:1::");
    c->ConfigureUpstream();assert(!c->addressPeers_[0].ipv6Routed);
    c->addressPeers_[1].ipv6.SetGatewayPrefix(prefix1);
    c->ConfigureUpstream();assert(c->addressPeers_[0].ipv6Routed);
    // NetId replacement on the same interface must also rebuild policy-rule ownership.
    ++net.netId;calls.clear();c->OnUpstreamChanged();q.Drain();
    assert(c->addressPeers_[0].upstreamNetId==net.netId);
    assert(std::find(calls.begin(),calls.end(),"iface-forward-del:sleip0:rmnet0")!=calls.end());
    // A peer pending release may not reacquire forwarding while its failed delete is retried.
    link.peerLinks[1].releasing=true;errors["nat-del:sleip1"]=-7;calls.clear();
    c->ReconcileGatewayPeers(link);assert(c->addressPeers_[1].releasing && c->addressPeers_[0].natEnabled);
    assert(std::find(calls.begin(),calls.end(),"iface-forward-add:sleip1:rmnet0")==calls.end());
    errors.clear();c->ReconcileGatewayPeers(link);assert(c->addressPeers_.count(1)==0);
    link.peerLinks[1].releasing=false;++link.peerLinks[1].generation;c->ReconcileGatewayPeers(link);
    assert(c->addressPeers_[1].natEnabled && !c->addressPeers_[1].releasing);
    // Removing slot zero releases exactly its pair; slot one keeps NAT and DNS.
    link.peerLinks.erase(link.peerLinks.begin());calls.clear();c->ReconcileGatewayPeers(link);
    assert(c->addressPeers_.size()==1 && c->addressPeers_[1].natEnabled && c->dnsProxyStarted_);
    assert(std::find(calls.begin(),calls.end(),"nat-del:sleip0:rmnet0")!=calls.end());
    assert(std::find(calls.begin(),calls.end(),"nat-del:sleip1:rmnet0")==calls.end());
    link.peerLinks.clear();c->ReconcileGatewayPeers(link);
    assert(c->addressPeers_.empty() && c->forwardingEnabled_ && c->dnsProxyStarted_);
    assert(c->StopGateway()==0);q.Drain();assert(!c->forwardingEnabled_ && !c->dnsProxyStarted_);
    net.hasDefault=false;net.upstreamProperties={};net.netId=10;net.iface="wlan0";
    puts("P3-S3 controller: per-interface NAT/forward/DNS, zero-peer holders, upstream loss/switch/partial retries, independent families and prefix collision PASS");
    // Reproduce a transient peer cleanup failure with two live peers. One click
    // keeps STOPPING/admission and completes automatically after the failure clears.
    activeGateway();errors["route-del"]=-EBUSY;calls.clear();
    assert(c->StopGateway()==0);q.Drain();
    assert(c->status_.state==NearlinkIpShareState::STOPPING && c->stopRequested_);
    assert(c->addressPeers_.size()==2 && c->gatewayReserved_);
    assert(c->StopGateway()==0 && q.tasks.empty()); // repeated click is idempotent
    assert(c->StartGatewayAny(3,2)!=0); // cannot start across an unfinished stop
    errors.clear();retryStop();
    assert(c->status_.state==NearlinkIpShareState::IDLE && c->addressPeers_.empty() && !c->gatewayReserved_);
    assert(std::count(calls.begin(),calls.end(),"nearlink-stop")==1);
    // A successful Stop IPC is not proof that the lower TUN/channels have drained.
    activeGateway();auto &lower=OHOS::Nearlink::NearlinkIpShareClient::GetInstance();lower.drainOnStop=false;
    assert(c->StopGateway()==0);q.Drain();
    assert(c->status_.state==NearlinkIpShareState::STOPPING && c->gatewayReserved_ && !c->nearlinkStarted_);
    lower.snapshot.role=NearlinkIpShareRole::NONE;lower.snapshot.state=NearlinkIpShareState::IDLE;
    retryStop();assert(c->status_.state==NearlinkIpShareState::IDLE && !c->gatewayReserved_);
    lower.drainOnStop=true;
    // Persistent resource failure remains ERROR/owned after the bounded retries.
    activeGateway();errors["ipv6-cleanup"]=-EACCES;
    assert(c->StopGateway()==0);q.Drain();retryStop();retryStop();retryStop();
    assert(q.delayed.empty() && c->status_.state==NearlinkIpShareState::ERROR);
    assert(c->status_.errorStage=="CLEANUP" && c->addressPeers_.size()==2 && c->gatewayReserved_);
    assert(!c->stopRequested_ && c->StartGatewayAny(3,2)!=0);
    errors.clear();assert(c->StopGateway()==0);q.Drain();assert(c->status_.state==NearlinkIpShareState::IDLE);
    // A delayed retry from a previous stop must not touch a new generation.
    activeGateway();errors["route-del"]=-EBUSY;assert(c->StopGateway()==0);q.Drain();
    auto staleRetry=q.delayed.front();q.delayed.clear();errors.clear();assert(c->Cleanup());
    activeGateway();calls.clear();staleRetry();q.Drain();
    assert(c->addressPeers_.size()==2 && std::find(calls.begin(),calls.end(),"nearlink-stop")==calls.end());
    assert(c->StopGateway()==0);q.Drain();assert(c->status_.state==NearlinkIpShareState::IDLE);
    puts("gateway stop: one-click retry, lower drain, idempotence, admission, bounded failure and stale retry PASS");
    c->status_.netId=106;
    c->status_.ipv4.configurationAvailable=c->status_.ipv6.configurationAvailable=true;
    c->status_.ipv4.externalAvailable=c->status_.ipv6.externalAvailable=true;
    c->status_.ipv4.validation=c->status_.ipv6.validation=2;
    assert(c->Cleanup(false));
    assert(c->status_.netId==-1 && !c->status_.ipv4.configurationAvailable && !c->status_.ipv6.configurationAvailable);
    assert(!c->status_.ipv4.externalAvailable && !c->status_.ipv6.externalAvailable);
    assert(c->status_.ipv4.validation==0 && c->status_.ipv6.validation==0);
    puts("cleanup without IDLE publication clears stale family availability/validation and netId PASS");
    puts("actual_controller: no-peer gateway capacity and zero-peer projection, dual merge, single supplier, independent failure/recovery, DNS withdrawal, per-peer DHCP/RA, slot-zero release, failure retention and address capacity PASS");
}
''')
    includes = [out, source / 'include', repo / 'interfaces/innerkits/netshareclient/include',
                workspace / 'tethering_dhcp/interfaces/kits/c']
    exe = out / 'check.exe'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', *[f'-I{p}' for p in includes],
                    str(out / 'test.cpp'), '-o', str(exe), '-lws2_32'], check=True)
    subprocess.run([str(exe)], check=True)
