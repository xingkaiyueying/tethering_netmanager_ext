"""Run actual RA assembly methods. Socket, FFRT and product integration are not covered."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
src = repo / 'services/networksharemanager'
with tempfile.TemporaryDirectory(prefix='p2-ra-') as directory:
    out = Path(directory)
    def put(name, text):
        p = out / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding='utf-8')
    for name in ('router_advertisement_daemon.h', 'router_advertisement_params.h'):
        put(name, (src / 'include' / name).read_text(encoding='utf-8'))
    put('netinet/in.h', '#pragma once\n#include <winsock2.h>\n#include <ws2tcpip.h>\n')
    put('arpa/inet.h', '#include <netinet/in.h>\n')
    put('sys/ioctl.h', '')
    put('sys/socket.h', '#include <netinet/in.h>\n')
    put('netmgr_ext_log_wrapper.h', '#define NETMGR_EXT_LOG_D(...) ((void)0)\n')
    put('securec.h', '''#pragma once
#include <cstring>
#define EOK 0
inline int memcpy_s(void*d,size_t n,const void*s,size_t k){if(k>n)return -1;memcpy(d,s,k);return 0;}
inline int memset_s(void*d,size_t n,int c,size_t k){if(k>n)return -1;memset(d,c,k);return 0;}
''')
    put('ffrt_timer.h', '''#pragma once
#include <shared_mutex>
namespace ffrt { using shared_mutex=std::shared_mutex; struct queue {}; using task_handle=void*; }
''')
    production = (src / 'src/router_advertisement_daemon.cpp').read_text(encoding='utf-8')
    methods = production[production.index('bool RouterAdvertisementDaemon::AssembleRaLocked()'):]
    params = (src / 'src/router_advertisement_params.cpp').read_text(encoding='utf-8')
    put('test.cpp', '''#include <memory>
#include <cassert>
#include <iostream>
#include <any>
#include <sstream>
#define private public
#include "router_advertisement_daemon.h"
#undef private
namespace OHOS { namespace NetManagerStandard {
constexpr size_t RA_HEADER_SIZE=16;
constexpr uint8_t DEFAULT_ROUTER_PRE=8, PREFIX_INFO_FLAGS=0xc0;
constexpr uint32_t HW_MAC_STR_LENGTH=17;
RouterAdvertisementDaemon::RouterAdvertisementDaemon(){raParams_=std::make_shared<RaParams>();}
RouterAdvertisementDaemon::~RouterAdvertisementDaemon(){}
''' + methods + '\n' + params + '''
int main() {
    using namespace OHOS::NetManagerStandard;
    RouterAdvertisementDaemon d;
    auto &p=*d.raParams_; p.layer3_=true; p.mtu_=1500; p.routerLifetime_=0; p.rdnssLifetime_=17;
    p.macAddr_="02:11:22:33:44:55";
    IpPrefix prefix; prefix.prefix.s6_addr[0]=0xfd; prefix.prefixesLength=64;
    prefix.preferredLifetime=5; prefix.validLifetime=11; p.prefixes_.push_back(prefix);
    in6_addr dns{}; dns.s6_addr[0]=0x20; dns.s6_addr[1]=1; dns.s6_addr[15]=0x53; p.dnses_.push_back(dns);
    assert(d.AssembleRaLocked());
    auto b=d.raPacket_; assert(d.raPacketLength_==88 && b[6]==0 && b[7]==0);
    assert(b[16]==1 && b[17]==1 && b[18]==2 && b[23]==0x55);
    assert(b[32]==3 && b[39]==11 && b[43]==5);
    assert(b[64]==25 && b[65]==3 && b[71]==17 && b[72]==0x20 && b[87]==0x53);
    p.prefixes_[0].preferredLifetime=12; assert(!d.AssembleRaLocked());
    p.prefixes_[0].preferredLifetime=5; p.dnses_.resize(5); assert(!d.AssembleRaLocked());
    p.dnses_.clear(); assert(d.AssembleRaLocked() && d.raPacketLength_==64);
    p.layer3_=false; assert(d.AssembleRaLocked() && d.raPacketLength_==88);
    std::cout<<"RA assembly: independent router/PIO/RDNSS lifetimes, explicit DNS, bounds, legacy PASS\\n";
}
''')
    subprocess.run(['g++', '-std=c++17', '-I'+str(out), str(out/'test.cpp'), '-lws2_32', '-o', str(out/'test.exe')], check=True)
    subprocess.run([str(out/'test.exe')], check=True)
    # Compile the actual socket/lifecycle and probe translation units; these are declarations-only OS stubs.
    put('netmgr_ext_log_wrapper.h', '\n'.join('#define NETMGR_EXT_LOG_'+level+'(...) ((void)0)' for level in ['D','I','E','W']))
    put('net_manager_constants.h', 'enum { NETMANAGER_EXT_SUCCESS=0, NETMANAGER_EXT_ERR_MEMSET_FAIL=-1, NETMANAGER_EXT_ERR_PARAMETER_ERROR=-2 };')
    put('ffrt_timer.h', '''#pragma once
#include <shared_mutex>
namespace ffrt {using shared_mutex=std::shared_mutex;using task_handle=void*;
struct task_attr {task_attr &delay(unsigned){return *this;}};
struct queue {queue(const char*){} template<class...T>task_handle submit_h(T...){return nullptr;} void cancel(task_handle){}};
}
''')
    put('net/if.h', '''#pragma once
struct ifreq {char ifr_name[16];short ifr_flags;};
#undef IFF_UP
#undef IFF_NOARP
#undef IFF_MULTICAST
#undef IFNAMSIZ
enum {IFF_UP=1,IFF_NOARP=128,IFF_MULTICAST=4096,IFNAMSIZ=16};
unsigned if_nametoindex(const char*);
''')
    put('ifaddrs.h', '''#pragma once
#include <sys/socket.h>
struct ifaddrs {ifaddrs *ifa_next; char *ifa_name; sockaddr *ifa_addr;};
int getifaddrs(ifaddrs **);
void freeifaddrs(ifaddrs *);
''')
    put('sys/ioctl.h', 'enum {SIOCGIFFLAGS=1,SIOCSIFFLAGS=2};\nint ioctl(int,unsigned long,...);\n')
    put('sys/wait.h', '#define WIFEXITED(s) (true)\n#define WEXITSTATUS(s) (s)\nint waitpid(int,int*,int);\nint fork();\n')
    put('arpa/inet.h', '#include <netinet/in.h>\nint inet_pton(int,const char*,void*);\nconst char* inet_ntop(int,const void*,char*,size_t);\n')
    put('netinet/icmp6.h', '')
    put('sys/socket.h', '''#pragma once
#include <netinet/in.h>
#include <cstddef>
#include <unistd.h>
struct iovec{void*iov_base;size_t iov_len;};
struct cmsghdr{size_t cmsg_len;int cmsg_level,cmsg_type;};
struct msghdr{void*msg_name;int msg_namelen;iovec*msg_iov;size_t msg_iovlen;void*msg_control;size_t msg_controllen;int msg_flags;};
#define CMSG_SPACE(n) (sizeof(cmsghdr)+(n))
#define CMSG_LEN(n) CMSG_SPACE(n)
#define CMSG_FIRSTHDR(m) ((cmsghdr*)(m)->msg_control)
#define CMSG_NXTHDR(m,c) nullptr
#undef CMSG_DATA
#define CMSG_DATA(c) ((char*)(c)+sizeof(cmsghdr))
#ifndef SO_BINDTODEVICE
#define SO_BINDTODEVICE 25
#endif
#ifndef IPV6_RECVHOPLIMIT
#define IPV6_RECVHOPLIMIT 51
#endif
#ifndef MSG_CTRUNC
#define MSG_CTRUNC 8
#endif
#ifndef MSG_TRUNC
#define MSG_TRUNC 32
#endif
int linux_setsockopt(int,int,int,const void*,size_t);
int linux_sendto(int,const void*,size_t,int,const sockaddr*,size_t);
ssize_t recvmsg(int,msghdr*,int);
#define setsockopt linux_setsockopt
#define sendto linux_sendto
#define pthread_setname_np(handle,name) ((void)0)
''')
    with (out/'securec.h').open('a', encoding='utf-8') as file:
        file.write('inline int strncpy_s(char*d,size_t n,const char*s,size_t k){if(k>=n)return -1;memcpy(d,s,k);d[k]=0;return 0;}\n')
    fixture = repo/'test/mytest/sleip_ipv6_config.cpp'
    fixture_text = fixture.read_text(encoding='utf-8')
    assert 'if (HasAddress(address)) return true;' in fixture_text
    assert 'owner.KeepAddresses();' in fixture_text
    assert 'gateway_retained_until_interface_cleanup=1' in fixture_text
    for source in (src/'src/router_advertisement_daemon.cpp', fixture):
        subprocess.run(['g++','-std=c++17','-include','memory','-fsyntax-only','-I'+str(out),str(source)],check=True)
    print('RA daemon and native IPv6 fixture syntax/retained-address policy=PASS (OS/FFRT boundary stubbed)')
