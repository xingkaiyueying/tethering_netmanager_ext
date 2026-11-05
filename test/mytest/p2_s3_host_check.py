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
        # Preserve production arguments; map HiLog privacy annotations to printf for host diagnostics.
        if name.endswith('.cpp'):
            text = text.replace('%{public}', '%').replace('%{private}', '%')
        file.write_text(text, encoding='utf-8')
    put('mock.h', (Path(__file__).with_name('p2_s3_platform_mock.h')).read_text())
    put('parcel.h', '''#pragma once
#include <string>
#include <cstdint>
class Parcel { public:
size_t GetDataSize()const{return 0;}
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
    put('ifaddrs.h', '#pragma once\nstruct ifaddrs {ifaddrs *ifa_next;char *ifa_name;sockaddr *ifa_addr,*ifa_netmask;};\ninline int getifaddrs(ifaddrs **p){*p=nullptr;return 0;}\ninline void freeifaddrs(ifaddrs*){}\n')
    put('nearlink_ipv6_runtime.h', '''#pragma once
#include "mock.h"
namespace OHOS::NetManagerStandard {
class NearlinkIpv6Runtime { public:
bool Prepare(bool,const std::string&,const std::string& = ""){return call("ipv6-prepare")==0;}
void PrepareGatewayAddress(){call("ipv6-address-prepare");}
std::string prefix;bool routed=false;
inline static unsigned upstreamEpoch=0;
inline static bool kernelV4Default=false;
static bool HasDefaultRouteOnInterface(const std::string &iface,int family){return family==AF_INET && kernelV4Default && iface=="rmnet0";}
bool HasPrefix()const{return !prefix.empty();}
const std::string &CurrentPrefix()const{return prefix;}
const std::string &LinkLocal()const{static std::string a="fe80::1";return a;}
const std::string &Gateway()const{static std::string address="2001:db8::1";return address;}
void SetGatewayPrefix(const std::string &p){prefix=p;}
bool OwnsPrefix(const std::string &p)const{return !p.empty()&&prefix==p;}
bool HasDefaultRouter()const{return routed;}
static std::string DeriveGatewayPrefix(const NetLinkInfo *u){return u ? "2001:db8:"+std::to_string(10+upstreamEpoch)+":"+std::to_string(1)+"::" : "";}
bool Advertise(const NetLinkInfo*,bool f,bool){routed=f;return call("ra")==0;}
bool Cleanup(){return call("ipv6-cleanup")==0;}
}; }
''')
    # Copy only the controller header so the platform runtime double is selected explicitly.
    put('nearlink_ipshare_controller.h', (source / 'include/nearlink_ipshare_controller.h').read_text())
    put('nearlink_ipshare_controller.cpp', (source / 'src/nearlink_ipshare_controller.cpp').read_text())
    if os.environ.get('P3_EVENT_SNAPSHOT_NEGATIVE'):
        text = (out / 'nearlink_ipshare_controller.cpp').read_text(encoding='utf-8')
        assert text.count('self->HandleNearlinkStatus(current);') == 1
        put('nearlink_ipshare_controller.cpp', text.replace('self->HandleNearlinkStatus(current);',
                                                         'self->HandleNearlinkStatus(status);'))
    stop_before = os.environ.get('P3_LOWER_STOP_BEFORE_REF')
    if stop_before:
        payload = subprocess.check_output(['git', '-c', f'safe.directory={repo.as_posix()}', '-C', str(repo),
            'show', stop_before + ':services/networksharemanager/src/nearlink_ipshare_controller.cpp'])
        (out / 'nearlink_ipshare_controller.cpp').write_bytes(payload)
    put('nearlink_peer_address.cpp', (source / 'src/nearlink_peer_address.cpp').read_text())
    if os.environ.get('P3_MEMBER_LOG_FORMAT_NEGATIVE'):
        text = (out / 'nearlink_peer_address.cpp').read_text(encoding='utf-8')
        assert text.count('member removed slot=%u epoch=%llu') == 1
        put('nearlink_peer_address.cpp', text.replace('member removed slot=%u epoch=%llu',
                                                    'member removed epoch=%llu'))
    before_ref = os.environ.get('P3_SWITCH_BEFORE_REF')
    upstream_source = (source / 'src/nearlink_peer_upstream.cpp').read_text()
    if before_ref:
        upstream_source = subprocess.check_output(['git', '-c', f'safe.directory={repo.as_posix()}',
            '-C', str(repo), 'show', before_ref + ':services/networksharemanager/src/nearlink_peer_upstream.cpp']).decode()
    put('nearlink_peer_upstream.cpp', upstream_source)
    put('nearlink_upstream_status.cpp', (source / 'src/nearlink_upstream_status.cpp').read_text())
    if os.environ.get('P3_UPSTREAM_CONTROLLER_NEGATIVE'):
        signal = (out / 'nearlink_upstream_status.cpp').read_text()
        assert signal.count('++validation_[i].revision;') == 1
        put('nearlink_upstream_status.cpp', signal.replace('++validation_[i].revision;', ''))
    put('nearlink_upstream_signal.h', '''#pragma once
#include "mock.h"
namespace OHOS::NetManagerStandard {
class NearlinkUpstreamSignal {
public:
 static constexpr uint8_t IPV4=1,IPV6=2;
 struct Link {std::string iface,localIpv4,gatewayIpv4;uint32_t ifindex=0;int32_t netId=-1;bool ipv6=false;std::string localIpv6,gatewayIpv6;
 struct Member{uint64_t generation;std::string ipv4;std::vector<std::string> ipv6;};std::vector<Member> members;};
 struct State {bool known=false;uint8_t available=0,changed=0;bool reconfigureIpv4=false;};
 static State remote;
 inline static std::map<std::string,uint8_t> announced;
 void Reset() {}
 void Gateway(const Link &link,uint8_t mask) {announced[link.iface]=mask;}
 State Terminal(const Link &) {auto result=remote;remote.changed=0;return result;}
};
inline NearlinkUpstreamSignal::State NearlinkUpstreamSignal::remote{};
}
''')
    restart_before = os.environ.get('P3_RESTART_BEFORE_REF')
    if restart_before:
        for name in ('nearlink_ipshare_controller.cpp', 'nearlink_peer_upstream.cpp'):
            payload = subprocess.check_output(['git', '-c', f'safe.directory={repo.as_posix()}',
                '-C', str(repo), 'show', restart_before + ':services/networksharemanager/src/' + name])
            (out / name).write_bytes(payload)
    prefix_before = os.environ.get('P3_RESTART_PREFIX_BEFORE_REF')
    if prefix_before:
        payload = subprocess.check_output(['git', '-c', f'safe.directory={repo.as_posix()}',
            '-C', str(repo), 'show', prefix_before + ':services/networksharemanager/src/nearlink_peer_upstream.cpp'])
        (out / 'nearlink_peer_upstream.cpp').write_bytes(payload)
    validation_before = os.environ.get('P3_VALIDATION_BEFORE_REF')
    if validation_before:
        for name in ('include/nearlink_ipshare_controller.h', 'src/nearlink_ipshare_controller.cpp'):
            payload = subprocess.check_output(['git', '-c', f'safe.directory={repo.as_posix()}',
                '-C', str(repo), 'show', validation_before + ':services/networksharemanager/' + name])
            (out / Path(name).name).write_bytes(payload)
    # Control detached probe completion order while executing the production controller.
    controller = (out / 'nearlink_ipshare_controller.cpp').read_text()
    negative = os.environ.get('P3_CONFIG_TIMING_NEGATIVE')
    if negative == 'probe':
        controller = controller.replace('supplierAvailable_ = supplier->isAvailable_;\n    RequestFamilyValidation();',
                                        'supplierAvailable_ = supplier->isAvailable_;')
    if negative == 'wake':
        start = controller.index('void NearlinkIpShareController::OnInterfaceAddressChanged(')
        end = controller.index('void NearlinkIpShareController::OnUpstreamChanged()', start)
        controller = controller[:start] + 'void NearlinkIpShareController::OnInterfaceAddressChanged(const std::string &) {}\n\n' + controller[end:]
    if negative == 'prefix':
        text = (out / 'nearlink_peer_upstream.cpp').read_text()
        (out / 'nearlink_peer_upstream.cpp').write_text(text.replace('    peer.ipv6.PrepareGatewayAddress();', ''))
    fast_config = 'void NearlinkIpShareController::RequestFamilyValidation()' in controller
    fast_prefix = 'PrepareGatewayPeerIpv6(' in (out / 'nearlink_peer_upstream.cpp').read_text()
    tracker = (source / 'src/networkshare_tracker.cpp').read_text()
    callbacks = tracker[tracker.index('int32_t NetworkShareTracker::NetsysCallback::OnInterfaceAddressUpdated('):
                        tracker.index('int32_t NetworkShareTracker::NetsysCallback::OnInterfaceAdded(')]
    if not fast_config:
        callbacks = callbacks.replace('NearlinkIpShareController::GetInstance()->OnInterfaceAddressChanged(iface);', '')
    put('address_callbacks.cpp', 'namespace OHOS::NetManagerStandard {\n' + callbacks + '\n}\n')
    controller = controller.replace('std::thread([self, netId, ipv6, i,',
                                    'ProbeThread(ipv6, [self, netId, ipv6, i,')
    (out / 'nearlink_ipshare_controller.cpp').write_text(controller.replace('std::thread([', 'ProbeThread(['))
    put('nearlink_family_validation.h', '''#pragma once
#include <functional>
#include <deque>
struct ProbeJob {bool ipv6;std::function<void()> run;void operator()(){run();}};
inline std::deque<ProbeJob> probeThreads;
inline ProbeJob TakeProbe(bool ipv6){auto it=std::find_if(probeThreads.begin(),probeThreads.end(),
 [=](const auto &job){return job.ipv6==ipv6;});assert(it!=probeThreads.end());auto job=*it;probeThreads.erase(it);return job;}
struct ProbeThread {
template<class F> ProbeThread(bool ipv6,F f){probeThreads.push_back({ipv6,f});}
template<class F> ProbeThread(F f):ProbeThread(false,f){} void detach() {}};
namespace OHOS::NetManagerStandard {
struct NearlinkFamilyValidation { int ipv4=0,ipv6=0; };
inline int probe4=2,probe6=3;
inline NearlinkFamilyValidation ValidateNearlinkFamilies(int,bool v4,bool v6) {
    return {v4 ? probe4 : 0, v6 ? probe6 : 0};
} }
''')
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
    put('test.cpp', 'constexpr bool fastConfig=' + ('true' if fast_config else 'false') + ';\n' +
        'constexpr bool fastPrefix=' + ('true' if fast_prefix else 'false') + ';\n' + r'''
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
#include "nearlink_upstream_status.cpp"
#include "address_callbacks.cpp"
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
    c->OnIpv6Addresses("sleip0",addresses,c->generation_); q.Drain();
    DhcpResult v6{}; v6.iptype=1; strcpy(v6.strOptRouter1,"fe80::1");
    v6.ipv6LifeTime.routerLifeTime=180; v6.dnsList.dnsNumber=1;
    strcpy(v6.dnsList.dnsAddr[0],"fd77:77:1::53");
    c->OnDhcpSuccess(0,"sleip0",v6,c->generation_); q.Drain();
    if(fastConfig)assert(probeThreads.size()==1 && probeThreads.front().ipv6);
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
    c->OnDhcpSuccess(0,"sleip0",v4,c->generation_); q.Drain();
    assert(net.lastLink.netAddrList_.size()==2 && net.lastLink.routeList_.size()==4);
    assert(c->netSupplierId_==supplier && net.registrations==1);
    // A slow/stale IPv6 probe cannot clear or delay an unchanged IPv4 result.
    c->ValidateFamilies();assert(probeThreads.size()==2);
    auto complete4=TakeProbe(false);
    complete4();q.Drain();
    assert(c->status_.ipv4.validation==2 && c->status_.ipv4.externalAvailable);
    assert(c->status_.ipv6.validation==1);
    strcpy(v6.dnsList.dnsAddr[0],"fd77:77:1::54");
    c->OnDhcpSuccess(0,"sleip0",v6,c->generation_);q.Drain();
    assert(c->status_.ipv4.validation==2 && c->status_.ipv4.externalAvailable);
    auto stale6=TakeProbe(true);stale6();q.Drain();
    assert(c->status_.ipv6.validation==(fastConfig ? 1 : 0) && c->status_.ipv4.validation==2);
    c->ValidateFamilies();assert(probeThreads.size()==1);
    auto complete6=TakeProbe(true);complete6();q.Drain();
    assert(c->status_.ipv6.validation==3 && !c->status_.ipv6.externalAvailable);
    assert(c->status_.ipv4.validation==2 && c->status_.ipv4.externalAvailable);
    // A genuine fresh IPv4 failure must still replace PASS after its own DNS changes.
    probe4=3;strcpy(v4.strOptDns1,"192.168.77.53");
    c->OnDhcpSuccess(0,"sleip0",v4,c->generation_);q.Drain();c->ValidateFamilies();
    assert(probeThreads.size()==1);
    complete4=TakeProbe(false);complete4();q.Drain();
    assert(c->status_.ipv4.validation==3 && !c->status_.ipv4.externalAvailable);
    assert(c->status_.ipv6.validation==3);
    // Network identity changes invalidate both families even with identical link properties.
    net.terminalNetId=43;assert(c->PublishTerminalNetwork());
    assert(c->status_.netId==43 && c->status_.ipv4.validation==0 && c->status_.ipv6.validation==0);
    errors["netid-lookup"]=-7;assert(c->PublishTerminalNetwork());assert(c->status_.netId==-1);
    net.terminalNetId=42;errors.clear();c->ValidateFamilies();
    assert(c->status_.netId==42 && probeThreads.size()==2);
    complete4=TakeProbe(false);complete4();q.Drain();
    complete6=TakeProbe(true);complete6();q.Drain();
    strcpy(v4.strOptDns1,"192.168.77.1");probe4=2;
    c->OnDhcpSuccess(0,"sleip0",v4,c->generation_);q.Drain();c->ValidateFamilies();
    assert(probeThreads.size()==1);
    // A stopped session cannot publish a late PASS into the next session.
    auto late4=TakeProbe(false);
    puts("per-family validation: independent completion, IPv6 changes retain IPv4 PASS, stale family/session rejection, genuine failure retained PASS");
    { Parcel parcel; assert(c->status_.Marshalling(parcel)); }
    c->OnDhcpFailure(4,"sleip0","renew",c->generation_); q.Drain();
    assert(net.lastLink.netAddrList_.size()==2);
    c->OnDhcpFailure(1,"sleip0","lost",c->generation_); q.Drain();
    assert(net.lastLink.netAddrList_.size()==1 && net.lastLink.netAddrList_[0].family_==AF_INET6);
    assert(c->netSupplierId_==supplier && c->nearlinkStarted_);
    c->OnDhcpSuccess(0,"sleip0",v4,c->generation_); q.Drain();
    v6.dnsList.dnsNumber=0; c->OnDhcpSuccess(0,"sleip0",v6,c->generation_); q.Drain();
    assert(net.lastLink.dnsList_.size()==1 && c->status_.ipv4.configurationAvailable);
    assert(!c->status_.ipv6.configurationAvailable);
    addresses.addressCount=0; c->OnIpv6Addresses("sleip0",addresses,c->generation_);
    c->OnDhcpSuccess(0,"sleip0",v6,c->generation_); q.Drain();
    assert(net.lastLink.netAddrList_.size()==1 && net.lastLink.netAddrList_[0].family_==AF_INET);
    const auto &e=OHOS::Nearlink::NearlinkIpShareClient::GetInstance().evidence;
    assert(e.size()==2 && e.back().validLifetime==0);
    // A failed family update restores the committed aggregate without deleting the other family.
    errors["link-info"]=-7; strcpy(v4.strOptClientId,"192.168.77.3");
    c->OnDhcpSuccess(0,"sleip0",v4,c->generation_); q.Drain();
    assert(net.lastLink.netAddrList_[0].address_=="192.168.77.2" && c->netSupplierId_==supplier);
    auto expiry=c->pendingLeaseExpiry_;
    errors.clear();
    auto starts=std::count(calls.begin(),calls.end(),"dhcp-start");
    c->RetryTerminalNetwork();
    assert(!c->ipv4PublishPending_ && c->leaseExpiry_==expiry);
    assert(std::count(calls.begin(),calls.end(),"dhcp-start")==starts);
    assert(net.lastLink.netAddrList_[0].address_=="192.168.77.3");
    c->OnDhcpSuccess(0,"sleip0",v4,c->generation_); c->StopTerminal(); q.Drain();
    assert(c->status_.state==NearlinkIpShareState::IDLE && !c->nearlinkStarted_);
    late4();q.Drain();assert(c->status_.ipv4.validation==0 && !c->status_.ipv4.externalAvailable);
    c->OnDhcpSuccess(0,"sleip0",v4,c->generation_); q.Drain();
    assert(c->netSupplierId_==0);
    errors["link-info"]=-7;
    assert(c->StartTerminal(peer,3)==0);q.Drain();
    link.generation=2;link.sequence=1;OHOS::Nearlink::NearlinkIpShareClient::GetInstance().snapshot=link;
    c->OnNearlinkStatus(link);q.Drain();c->OnDhcpSuccess(0,"sleip0",v4,c->generation_);q.Drain();
    assert(c->ipv4PublishPending_ && !c->retryIpv4_ && c->netSupplierId_==0);
    expiry=c->pendingLeaseExpiry_;errors.clear();c->RetryTerminalNetwork();
    assert(c->status_.ipv4.configurationAvailable && c->leaseExpiry_==expiry);
    errors["link-info"]=-7;c->OnDhcpSuccess(0,"sleip0",v4,c->generation_);q.Drain();
    // Force a different address so the normal equality fast path is not used.
    strcpy(v4.strOptClientId,"192.168.77.4");c->OnDhcpSuccess(0,"sleip0",v4,c->generation_);q.Drain();
    c->pendingLeaseExpiry_=std::chrono::steady_clock::now()-std::chrono::seconds(1);
    errors.clear();c->RetryTerminalNetwork();assert(!c->ipv4PublishPending_&&c->retryIpv4_);
    c->StopTerminal();q.Drain();
    c->Cleanup();net.hasDefault=false;
    auto oldSession=session;
    errors["ipv6-prepare"]=-1;
    assert(c->StartTerminal(peer,3)==0);q.Drain();
    link.generation=3;link.sequence=1;OHOS::Nearlink::NearlinkIpShareClient::GetInstance().snapshot=link;
    c->OnNearlinkStatus(link);q.Drain();
    c->OnDhcpSuccess(0,"sleip0",v4,oldSession);q.Drain();assert(c->netSupplierId_==0);
    c->OnDhcpSuccess(0,"sleip0",v4,c->generation_);q.Drain();
    assert(c->status_.ipv4.configurationAvailable && c->nearlinkStarted_);
    q.delayed.clear();errors.clear();errors["ipv6-cleanup"]=-1;c->StopTerminal();q.Drain();
    for(int n=0;n<3;++n) {assert(!q.delayed.empty());auto retry=q.delayed.front();q.delayed.pop_front();retry();q.Drain();}
    assert(c->status_.state==NearlinkIpShareState::ERROR && c->status_.errorStage=="CLEANUP");
    errors.clear();c->StopTerminal();q.Drain();assert(c->status_.state==NearlinkIpShareState::IDLE);
    int32_t supported=0;assert(c->GetSupportedMaxTerminals(supported)==0 && supported==2);
    assert(c->StartGatewayAny(1,3)!=0);
    auto &capacityClient=OHOS::Nearlink::NearlinkIpShareClient::GetInstance();
    capacityClient.supportedMaxTerminals=32;
    assert(c->GetSupportedMaxTerminals(supported)==0 && supported==32);
    assert(c->StartGatewayAny(1,0)!=0 && c->StartGatewayAny(1,33)!=0);
    for (int capacity : {1,2,3,5,7,32}) {
        assert(c->StartGatewayAny(1,capacity)==0);q.Drain();
        assert(c->status_.maxTerminals==capacity && c->status_.supportedMaxTerminals==32 &&
               capacityClient.gatewayCapacity==capacity);
        assert(c->StartGatewayAny(1,capacity==32 ? 31 : capacity+1)!=0); // running limit stays fixed
        assert(c->StopGateway()==0);q.Drain();
        assert(c->status_.state==NearlinkIpShareState::IDLE);
    }

    capacityClient.supportedMaxTerminals=2;
    auto &lower=OHOS::Nearlink::NearlinkIpShareClient::GetInstance();
    uint64_t gatewaySerial=100;
    auto retryStop=[&]() {assert(!q.delayed.empty());auto task=q.delayed.front();q.delayed.pop_front();task();q.Drain();};
    auto activeGateway=[&]() {
        q.delayed.clear();
        assert(c->StartGatewayAny(3,2)==0);q.Drain();
        link={};link.role=NearlinkIpShareRole::GATEWAY;link.state=NearlinkIpShareState::CHANNEL_READY;
        link.generation=++gatewaySerial;link.sequence=1;link.serviceReady=true;link.selectedMode=3;
        link.peerLinks={{0,link.generation*2,3,"sleip0"},{1,link.generation*2+1,3,"sleip0"}};
        link.peerLinks[0].ipv4Address="172.24.0.2";link.peerLinks[0].ipv6Addresses={{"fd77::2"},{"fe80::2"}};
        link.peerLinks[1].ipv4Address="172.24.0.3";link.peerLinks[1].ipv6Addresses={{"fd77::3"},{"fe80::3"}};
        lower.snapshot=link;c->OnNearlinkStatus(link);q.Drain();q.delayed.clear();
        assert(c->addressPeers_.size()==2&&c->gatewayNetwork_.dhcpStarted&&c->gatewayReserved_);
    };
    q.delayed.clear();c->configuration_.pool4="172.24.0.0/24";
    assert(c->StartGatewayAny(3,2)==0);q.Drain();
    link={};link.role=NearlinkIpShareRole::GATEWAY;link.state=NearlinkIpShareState::SERVING_NO_UPSTREAM;
    link.generation=++gatewaySerial;link.sequence=1;link.serviceReady=true;link.selectedMode=3;
    lower.snapshot=link;calls.clear();c->OnNearlinkStatus(link);q.Drain();
    assert(c->addressPeers_.empty()&&c->gatewayNetwork_.dhcpStarted&&c->gatewayNetwork_.interfaceAdded);
    assert(std::count(calls.begin(),calls.end(),"server-start")==1);
    link.peerLinks={{0,201,3,"sleip0"},{1,202,1,"sleip0"}};
    link.peerLinks[0].ipv4Address="172.24.0.2";link.peerLinks[0].ipv6Addresses={{"fd77::2"},{"fe80::2"}};
    link.peerLinks[1].ipv4Address="172.24.0.3";
    ++link.sequence;lower.snapshot=link;calls.clear();c->OnNearlinkStatus(link);q.Drain();
    assert(c->addressPeers_.size()==2&&c->status_.ValidPeers());
    assert(std::count(calls.begin(),calls.end(),"server-start")==0);
    assert(c->status_.peers[0].ifaceName=="sleip0"&&c->status_.peers[1].ifaceName=="sleip0");
    assert(c->status_.peers[0].ipv4.addresses[0].address=="172.24.0.2");
    assert(c->status_.peers[1].ipv4.addresses[0].address=="172.24.0.3");
    assert(c->status_.peers[0].ipv6.configurationAvailable&&!c->status_.peers[1].ipv6.configurationAvailable);
    link.peerLinks.erase(link.peerLinks.begin());++link.sequence;lower.snapshot=link;
    calls.clear();c->ReconcileGatewayPeers(link);
    assert(c->addressPeers_.size()==1&&c->gatewayNetwork_.dhcpStarted&&!c->gatewayNetwork_.ipv6Prepared);
    assert(std::count(calls.begin(),calls.end(),"server-stop")==0);
    link.peerLinks.clear();++link.sequence;lower.snapshot=link;c->ReconcileGatewayPeers(link);
    assert(c->gatewayNetwork_.dhcpStarted&&c->gatewayNetwork_.interfaceAdded&&c->status_.ifaceName=="sleip0");
    assert(c->StopGateway()==0);q.Drain();assert(!c->gatewayNetwork_.generation);
    c->configuration_.pool4="172.24.0.0/16";
    net.hasDefault=true;net.upstreamProperties={};net.iface="wlan0";net.netId=10;
    Route def4;def4.destination_.family_=AF_INET;def4.destination_.prefixlen_=0;
    net.upstreamProperties.routeList_={def4};calls.clear();activeGateway();
    assert(std::count(calls.begin(),calls.end(),"nat-add")==1);
    assert(std::count(calls.begin(),calls.end(),"iface-forward-add")==1);
    assert(c->gatewayNetwork_.ipv6Ready&&c->gatewayNetwork_.ipv6Routed);
    net.hasDefault=false;calls.clear();c->ConfigureUpstream();
    assert(c->gatewayNetwork_.dhcpStarted&&c->gatewayNetwork_.ipv6Ready&&!c->gatewayNetwork_.natEnabled);
    assert(std::count(calls.begin(),calls.end(),"server-stop")==0);
    net.hasDefault=true;net.iface="rmnet0";net.netId=11;errors["nat-add"]=-7;c->ConfigureUpstream();
    assert(!c->gatewayNetwork_.natEnabled&&c->gatewayNetwork_.natAttempted);
    errors.clear();c->ConfigureUpstream();assert(c->gatewayNetwork_.natEnabled);
    auto preservedPrefix=c->gatewayNetwork_.ipv6.CurrentPrefix();
    INetAddr conflict;conflict.family_=AF_INET;conflict.address_="172.24.0.20";conflict.prefixlen_=24;
    net.upstreamProperties.netAddrList_={conflict};calls.clear();c->ConfigureUpstream();
    assert(c->gatewayNetwork_.addresses.gateway=="172.24.1.1"&&c->gatewayNetwork_.retiredIpv4.size()==1);
    assert(c->gatewayNetwork_.ipv6.CurrentPrefix()==preservedPrefix&&c->gatewayNetwork_.ipv6Ready);
    assert(std::count(calls.begin(),calls.end(),"server-stop")==1);
    assert(std::count(calls.begin(),calls.end(),"server-start")==1);
    auto emptyEvent=link;++emptyEvent.sequence;
    for(auto &member:emptyEvent.peerLinks){member.ipv4Address.clear();member.ipv6Addresses.clear();}
    link.sequence=emptyEvent.sequence;lower.snapshot=link;calls.clear();
    c->OnNearlinkStatus(emptyEvent);q.Drain();
    assert(c->gatewayNetwork_.retiredIpv4.size()==1);
    assert(c->addressPeers_.at(0).ipv4Address==link.peerLinks[0].ipv4Address);
    assert(std::count(calls.begin(),calls.end(),"address-del")==0);
    assert(std::count(calls.begin(),calls.end(),"route-del")==0);
    ++emptyEvent.sequence;link.sequence=emptyEvent.sequence;lower.snapshot=link;
    errors["snapshot"]=-1;q.delayed.clear();c->OnNearlinkStatus(emptyEvent);q.Drain();
    assert(c->gatewayNetwork_.retiredIpv4.size()==1&&!q.delayed.empty());
    auto retrySnapshot=q.delayed.back();q.delayed.clear();errors.clear();retrySnapshot();q.Drain();
    assert(c->gatewayNetwork_.retiredIpv4.size()==1&&c->linkSequence_==link.sequence);
    auto staleEvent=emptyEvent;staleEvent.generation=link.generation-1;
    c->OnNearlinkStatus(staleEvent);q.Drain();assert(c->gatewayNetwork_.retiredIpv4.size()==1);
    puts("R02 complete event snapshot, failed query retry and stale event isolation PASS");
    link.peerLinks[0].ipv4Address="172.24.1.2";link.peerLinks[1].ipv4Address="172.24.1.3";
    ++link.sequence;lower.snapshot=link;c->ConfigureUpstream();assert(c->gatewayNetwork_.retiredIpv4.empty());
    assert(c->StopGateway()==0);q.Drain();net.hasDefault=false;net.upstreamProperties={};
    puts("shared network: zero/first/last members, shared DHCP/RA/NAT, member addresses, mixed modes, upstream loss, subnet migration and retry PASS");
    activeGateway();mockIfIndex=8;c->ConfigureUpstream();
    assert(c->status_.state==NearlinkIpShareState::ERROR&&c->addressPeers_.empty());
    assert(c->status_.errorStage=="SHARED_IFINDEX");mockIfIndex=7;
    assert(c->StopGateway()==0);q.Drain();
    // Reproduce a transient peer cleanup failure with two live peers. One click
    // keeps STOPPING/admission and completes automatically after the failure clears.
    activeGateway();errors["route-del"]=-EBUSY;calls.clear();
    assert(c->StopGateway()==0);q.Drain();
    assert(c->status_.state==NearlinkIpShareState::STOPPING && c->stopRequested_);
    assert(c->gatewayNetwork_.generation && c->gatewayReserved_);
    assert(c->StopGateway()==0 && q.tasks.empty()); // repeated click is idempotent
    assert(c->StartGatewayAny(3,2)!=0); // cannot start across an unfinished stop
    errors.clear();retryStop();
    assert(c->status_.state==NearlinkIpShareState::IDLE && c->addressPeers_.empty() && !c->gatewayReserved_);
    assert(std::count(calls.begin(),calls.end(),"nearlink-stop")==1);
    // A successful Stop IPC is not proof that the lower TUN/channels have drained.
    activeGateway();lower.drainOnStop=false;
    assert(c->StopGateway()==0);q.Drain();
    assert(c->status_.state==NearlinkIpShareState::STOPPING && c->gatewayReserved_ && c->nearlinkStarted_);
    lower.snapshot.role=NearlinkIpShareRole::NONE;lower.snapshot.state=NearlinkIpShareState::IDLE;
    retryStop();assert(c->status_.state==NearlinkIpShareState::IDLE && !c->gatewayReserved_);
    lower.drainOnStop=true;
    // A transient lower destroy/post failure requires another Stop, not just polling.
    activeGateway();lower.drainOnStop=false;calls.clear();
    assert(c->StopGateway()==0);q.Drain();
    assert(c->nearlinkStarted_ && std::count(calls.begin(),calls.end(),"nearlink-stop")==1);
    lower.drainOnStop=true;retryStop();
    assert(c->status_.state==NearlinkIpShareState::IDLE && !c->nearlinkStarted_ && !c->gatewayReserved_);
    assert(std::count(calls.begin(),calls.end(),"nearlink-stop")==2);
    // Persistent lower failure keeps ownership; a later user retry can recover.
    activeGateway();lower.drainOnStop=false;calls.clear();
    assert(c->StopGateway()==0);q.Drain();retryStop();retryStop();retryStop();
    assert(c->status_.state==NearlinkIpShareState::ERROR && c->nearlinkStarted_ && c->gatewayReserved_);
    assert(std::count(calls.begin(),calls.end(),"nearlink-stop")==4);
    lower.drainOnStop=true;assert(c->StopGateway()==0);q.Drain();
    assert(c->status_.state==NearlinkIpShareState::IDLE && !c->nearlinkStarted_ && !c->gatewayReserved_);
    // Persistent resource failure remains ERROR/owned after the bounded retries.
    activeGateway();errors["ipv6-cleanup"]=-EACCES;
    assert(c->StopGateway()==0);q.Drain();retryStop();retryStop();retryStop();
    assert(q.delayed.empty() && c->status_.state==NearlinkIpShareState::ERROR);
    assert(c->status_.errorStage=="CLEANUP" && c->gatewayNetwork_.generation && c->gatewayReserved_);
    assert(!c->stopRequested_ && c->StartGatewayAny(3,2)!=0);
    errors.clear();assert(c->StopGateway()==0);q.Drain();assert(c->status_.state==NearlinkIpShareState::IDLE);
    // A delayed retry from a previous stop must not touch a new generation.
    activeGateway();errors["route-del"]=-EBUSY;assert(c->StopGateway()==0);q.Drain();
    auto staleRetry=q.delayed.front();q.delayed.clear();errors.clear();assert(c->Cleanup());
    activeGateway();calls.clear();staleRetry();q.Drain();
    assert(c->addressPeers_.size()==2 && std::find(calls.begin(),calls.end(),"nearlink-stop")==calls.end());
    assert(c->StopGateway()==0);q.Drain();assert(c->status_.state==NearlinkIpShareState::IDLE);
    puts("gateway stop: one-click retry, lower drain, idempotence, admission, bounded failure and stale retry PASS");
    // Terminal STOP must wait for lower IDLE too; cancelling a queued START
    // must never create a fresh lower session after the user's stop intent.
    calls.clear(); assert(c->StartTerminal(peer,3)==0); assert(c->StopTerminal()==0);q.Drain();
    assert(std::find(calls.begin(),calls.end(),"nearlink-start")==calls.end());
    assert(c->status_.state==NearlinkIpShareState::IDLE);
    assert(c->StartTerminal(peer,3)==0);q.Drain();
    lower.snapshot.role=NearlinkIpShareRole::TERMINAL;
    lower.snapshot.state=NearlinkIpShareState::CHANNEL_READY; lower.drainOnStop=false;
    assert(c->StopTerminal()==0);q.Drain();
    assert(c->status_.state==NearlinkIpShareState::STOPPING && c->stopRequested_);
    lower.snapshot.role=NearlinkIpShareRole::NONE;lower.snapshot.state=NearlinkIpShareState::IDLE;
    retryStop();assert(c->status_.state==NearlinkIpShareState::IDLE);lower.drainOnStop=true;
    assert(c->StartTerminal(peer,3)==0);q.Drain();
    lower.snapshot.role=NearlinkIpShareRole::TERMINAL;
    lower.snapshot.state=NearlinkIpShareState::CHANNEL_READY;lower.drainOnStop=false;calls.clear();
    assert(c->StopTerminal()==0);q.Drain();assert(c->nearlinkStarted_);
    lower.drainOnStop=true;retryStop();
    assert(c->status_.state==NearlinkIpShareState::IDLE && !c->nearlinkStarted_);
    assert(std::count(calls.begin(),calls.end(),"nearlink-stop")==2);
    // Only locally observed STOPPING causes a bounded startup wait.
    auto startupBusy=[&](uint64_t gen) {
        OHOS::Nearlink::NearlinkIpShareStatus busy;
        busy.role=NearlinkIpShareRole::TERMINAL;busy.peerAddress=peer;busy.ifaceName="sleip0";
        busy.state=NearlinkIpShareState::ERROR;busy.generation=gen;busy.sequence=1;
        busy.selectedMode=3;busy.errorStage="iposl-config";busy.errorCode=-6;
        lower.snapshot=busy;c->HandleNearlinkStatus(busy);
    };
    lower.snapshot.state=NearlinkIpShareState::STOPPING;
    assert(c->StartTerminal(peer,3)==0);q.Drain();
    assert(c->terminalRestartPending_);
    lower.snapshot.role=NearlinkIpShareRole::NONE;lower.snapshot.state=NearlinkIpShareState::IDLE;
    calls.clear();retryStop();assert(std::count(calls.begin(),calls.end(),"nearlink-start")==1);
    assert(c->nearlinkStarted_ && c->status_.requestedMode==3 && !c->terminalRestartPending_);
    assert(c->StopTerminal()==0);q.Drain();
    lower.snapshot.state=NearlinkIpShareState::STOPPING;
    assert(c->StartTerminal(peer,3)==0);q.Drain();
    auto staleStart=q.delayed.back();q.delayed.clear();
    assert(c->StopTerminal()==0);q.Drain();calls.clear();staleStart();q.Drain();
    assert(std::find(calls.begin(),calls.end(),"nearlink-start")==calls.end());
    assert(c->status_.state==NearlinkIpShareState::STOPPING);
    lower.snapshot.role=NearlinkIpShareRole::NONE;lower.snapshot.state=NearlinkIpShareState::IDLE;
    retryStop();
    assert(c->status_.state==NearlinkIpShareState::IDLE);
    assert(c->StartTerminal(peer,3)==0);q.Drain();
    startupBusy(200);assert(c->status_.state==NearlinkIpShareState::ERROR && !c->terminalRestartPending_);
    assert(c->StopTerminal()==0);q.Drain();
    lower.snapshot.state=NearlinkIpShareState::STOPPING;
    assert(c->StartTerminal(peer,3)==0);q.Drain();
    retryStop();retryStop();retryStop();retryStop();
    assert(c->status_.state==NearlinkIpShareState::ERROR && !c->terminalRestartPending_);
    lower.snapshot.role=NearlinkIpShareRole::NONE;lower.snapshot.state=NearlinkIpShareState::IDLE;
    assert(c->StopTerminal()==0);q.Drain();
    // DAD/RA admission stays CONFIGURING for ten seconds, then errors. A
    // retiring first slot must not replace the remaining live peer's summary.
    net.hasDefault=true;net.upstreamProperties.routeList_={def4};activeGateway();
    errors["ra"]=-1;c->ConfigureUpstream();
    assert(c->status_.ipv6.phase==1 && !c->status_.ipv6.hasError);
    c->gatewayNetwork_.ipv6PendingSince=std::chrono::steady_clock::now()-std::chrono::seconds(11);
    c->ConfigureUpstream();assert(c->status_.ipv6.hasError && c->status_.ipv6.error.stage=="PREFIX");
    errors.clear();c->ConfigureUpstream();assert(!c->status_.ipv6.hasError);
    link.peerLinks.erase(link.peerLinks.begin());lower.snapshot=link;
    c->ConfigureUpstream();assert(!c->status_.ipv6.hasError && c->gatewayNetwork_.ipv6Routed);
    assert(c->StopGateway()==0);q.Drain();net.hasDefault=false;
    puts("terminal restart: local drain wait, generic rejection ends, mode retention, bounded failure and stop cancellation PASS");
    // A private gateway signal revokes only the affected family's cached PASS.
    probeThreads.clear();probe4=probe6=2;
    assert(c->StartTerminal(peer,3)==0);q.Drain();
    link.role=NearlinkIpShareRole::TERMINAL;link.state=NearlinkIpShareState::CHANNEL_READY;
    link.peerAddress=peer;link.ifaceName="sleip0";
    link.generation=900;link.sequence=1;link.selectedMode=3;
    OHOS::Nearlink::NearlinkIpShareClient::GetInstance().snapshot=link;
    c->OnNearlinkStatus(link);q.Drain();
    addresses.addressCount=1;c->OnIpv6Addresses("sleip0",addresses,c->generation_);q.Drain();
    v6.ipv6LifeTime.routerLifeTime=180;v6.dnsList.dnsNumber=1;
    strcpy(v6.dnsList.dnsAddr[0],"fd77:77:1::53");
    c->OnDhcpSuccess(0,"sleip0",v6,c->generation_);c->OnDhcpSuccess(0,"sleip0",v4,c->generation_);q.Drain();
    NearlinkUpstreamSignal::remote={true,3};
    assert(c->PollTerminalUpstream());c->RefreshFamilyStatus();q.Drain();
    while(!probeThreads.empty()){auto job=probeThreads.front();probeThreads.pop_front();job();q.Drain();}
    assert(c->status_.ipv4.externalAvailable && c->status_.ipv6.externalAvailable);
    auto localLink=net.lastLink;auto localSupplier=c->netSupplierId_;
    c->validation_[0].next={};c->ValidateFamilies();auto lateUpstream4=TakeProbe(false);
    NearlinkUpstreamSignal::remote={true,2};
    assert(c->PollTerminalUpstream());c->RefreshFamilyStatus();q.Drain();
    assert(!c->status_.ipv4.externalAvailable && c->status_.ipv4.validation==3);
    assert(c->status_.ipv4.error.stage=="UPSTREAM" && c->status_.ipv6.externalAvailable);
    // Recovery before the old probe completes still requires a fresh probe.
    NearlinkUpstreamSignal::remote={true,3};
    c->PollTerminalUpstream();c->RefreshFamilyStatus();q.Drain();
    lateUpstream4();q.Drain();assert(!c->status_.ipv4.externalAvailable);
    auto fresh4=TakeProbe(false);fresh4();q.Drain();assert(c->status_.ipv4.externalAvailable);
    NearlinkUpstreamSignal::remote={true,2};
    c->PollTerminalUpstream();c->RefreshFamilyStatus();q.Drain();
    assert(c->status_.hasUpstream && c->status_.state==NearlinkIpShareState::ACTIVE);
    NearlinkUpstreamSignal::remote={true,0};
    assert(c->PollTerminalUpstream());c->RefreshFamilyStatus();q.Drain();
    assert(!c->status_.hasUpstream && !c->status_.ipv6.externalAvailable);
    assert(c->status_.ipv4.configurationAvailable && c->status_.ipv6.configurationAvailable);
    assert(c->netSupplierId_==localSupplier && net.lastLink==localLink);
    c->ValidateFamilies();assert(probeThreads.empty());
    NearlinkUpstreamSignal::remote={true,1};
    assert(c->PollTerminalUpstream());c->RefreshFamilyStatus();q.Drain();
    assert(!c->status_.ipv4.externalAvailable && c->status_.ipv4.validation==1);
    auto restore4=TakeProbe(false);restore4();q.Drain();
    assert(c->status_.ipv4.externalAvailable && !c->status_.ipv6.externalAvailable);
    NearlinkUpstreamSignal::remote={true,3};
    assert(c->PollTerminalUpstream());c->RefreshFamilyStatus();q.Drain();
    auto restore6=TakeProbe(true);restore6();q.Drain();
    assert(c->status_.ipv4.externalAvailable && c->status_.ipv6.externalAvailable);
    NearlinkUpstreamSignal::remote={true,3,1};c->PollTerminalUpstream();c->RefreshFamilyStatus();q.Drain();
    assert(!c->status_.ipv4.externalAvailable && c->status_.ipv6.externalAvailable);
    auto flap4=TakeProbe(false);flap4();q.Drain();assert(c->status_.ipv4.externalAvailable);
    NearlinkUpstreamSignal::remote={true,0};c->PollTerminalUpstream();c->RefreshFamilyStatus();q.Drain();
    NearlinkUpstreamSignal::remote={};c->PollTerminalUpstream();c->RefreshFamilyStatus();q.Drain();
    assert(probeThreads.size()==2 && !c->status_.ipv4.externalAvailable && !c->status_.ipv6.externalAvailable);
    auto lateSignal=TakeProbe(false);TakeProbe(true);c->StopTerminal();q.Drain();lateSignal();q.Drain();
    assert(!c->gatewayUpstream_.known && c->status_.state==NearlinkIpShareState::IDLE);
    puts("remote upstream: IPv4/IPv6 isolated loss, late PASS rejection, retained local link, verified recovery, legacy fallback and stop reset PASS");
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
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror=format', '-Werror=format-extra-args',
                    *[f'-I{p}' for p in includes],
                    str(out / 'test.cpp'), '-o', str(exe), '-lws2_32'], check=True)
    subprocess.run([str(exe)], check=True)
