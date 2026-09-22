"""Compile the production controller and family aggregation against explicit platform doubles.
Does not prove Binder, kernel, product compilation, radio, routing, or DNS reachability.
"""
from pathlib import Path
import subprocess
import tempfile

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
    put('netinet/ip.h', '#pragma once\n#include <winsock2.h>\n#include <ws2tcpip.h>\n#undef ERROR\nextern "C" int inet_pton(int,const char*,void*);\nextern "C" const char* inet_ntop(int,const void*,char*,size_t);\n')
    put('arpa/inet.h', '#include <netinet/ip.h>\n')
    put('net/if.h', '#pragma once\ninline unsigned if_nametoindex(const char*) { return 7; }\n')
    put('nearlink_ipv6_runtime.h', '''#pragma once
#include "mock.h"
namespace OHOS::NetManagerStandard {
class NearlinkIpv6Runtime { public:
bool Prepare(bool,const std::string&){return call("ipv6-prepare")==0;}
bool Advertise(const NetLinkInfo*,bool,bool){return call("ra")==0;}
bool Cleanup(){return call("ipv6-cleanup")==0;}
}; }
''')
    # Copy only the controller header so the platform runtime double is selected explicitly.
    put('nearlink_ipshare_controller.h', (source / 'include/nearlink_ipshare_controller.h').read_text())
    put('nearlink_ipshare_controller.cpp', (source / 'src/nearlink_ipshare_controller.cpp').read_text())
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
    errors.clear();c->ConfigureUpstream();assert(c->status_.ipv6.configurationAvailable);
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
    puts("actual_controller: dual merge, single supplier, independent failure/recovery, DNS withdrawal, evidence, stop fence PASS");
}
''')
    includes = [out, source / 'include', repo / 'interfaces/innerkits/netshareclient/include',
                workspace / 'tethering_dhcp/interfaces/kits/c']
    exe = out / 'check.exe'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', *[f'-I{p}' for p in includes],
                    str(out / 'test.cpp'), '-o', str(exe), '-lws2_32'], check=True)
    subprocess.run([str(exe)], check=True)
