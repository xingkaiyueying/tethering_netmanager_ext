"""Run the production route dump with controlled kernel replies; no device kernel proof."""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
relative = 'services/networksharemanager/src/nearlink_ipv6_runtime.cpp'
source = (repo / relative).read_text(encoding='utf-8')
before = os.environ.get('P3_ROUTE_BEFORE_REF')
if before:
    source = subprocess.check_output(['git', '-c', 'safe.directory=' + repo.as_posix(),
                                     '-C', str(repo), 'show', before + ':' + relative]).decode()
signature = 'bool NearlinkIpv6Runtime::HasDefaultRouteOnInterface('
if signature in source:
    start = source.index(signature)
else:
    start = source.index('bool HasIpv6DefaultRouteOnInterface(')
brace = source.index('{', start)
end, depth = brace + 1, 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
method = source[start:end]
method = method.replace('bool HasIpv6DefaultRouteOnInterface(const std::string &iface)',
                        'bool NearlinkIpv6Runtime::HasDefaultRouteOnInterface(const std::string &iface, int32_t family)')

fixture = r'''
#define pollfd WinSockPollfd
#include <winsock2.h>
#undef pollfd
#undef POLLIN
#undef POLLERR
#include <cstdint>
#include <cstring>
#include <chrono>
#include <thread>
#include <string>
#include <cerrno>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <functional>
#include <deque>
using ssize_t=long long;
constexpr int AF_NETLINK=16, NETLINK_ROUTE=0, SOCK_CLOEXEC=0x80000, SOCK_NONBLOCK=0x800;
constexpr int MSG_DONTWAIT=0x40, POLLIN=1, POLLERR=8;
constexpr int NLM_F_REQUEST=1,NLM_F_DUMP=0x300,RTM_GETROUTE=26,RTM_NEWROUTE=24;
constexpr int NLMSG_DONE=3,NLMSG_ERROR=2,RTN_UNICAST=1,RTA_OIF=4;
struct nlmsghdr {uint32_t nlmsg_len;uint16_t nlmsg_type,nlmsg_flags;uint32_t nlmsg_seq,nlmsg_pid;};
struct rtmsg {uint8_t rtm_family,rtm_dst_len,rtm_src_len,rtm_tos,rtm_table,rtm_protocol,rtm_scope,rtm_type;uint32_t rtm_flags;};
struct rtattr {uint16_t rta_len,rta_type;};
struct sockaddr_nl {uint16_t nl_family;uint16_t pad;uint32_t nl_pid,nl_groups;};
struct pollfd {int fd;short events,revents;};
#define NLMSG_ALIGN(n) (((n)+3)&~3)
#define NLMSG_LENGTH(n) (NLMSG_ALIGN(sizeof(nlmsghdr))+(n))
#define NLMSG_DATA(p) ((char*)(p)+NLMSG_ALIGN(sizeof(nlmsghdr)))
#define NLMSG_OK(p,n) ((n)>=(int)sizeof(nlmsghdr)&&(p)->nlmsg_len>=sizeof(nlmsghdr)&&(p)->nlmsg_len<=(unsigned)(n))
#define NLMSG_NEXT(p,n) ((n)-=NLMSG_ALIGN((p)->nlmsg_len),(nlmsghdr*)((char*)(p)+NLMSG_ALIGN((p)->nlmsg_len)))
#define RTM_PAYLOAD(p) ((p)->nlmsg_len-NLMSG_LENGTH(sizeof(rtmsg)))
#define RTM_RTA(p) ((rtattr*)((char*)(p)+NLMSG_ALIGN(sizeof(rtmsg))))
#define RTA_OK(p,n) ((n)>=(int)sizeof(rtattr)&&(p)->rta_len>=sizeof(rtattr)&&(p)->rta_len<=(unsigned)(n))
#define RTA_NEXT(p,n) ((n)-=NLMSG_ALIGN((p)->rta_len),(rtattr*)((char*)(p)+NLMSG_ALIGN((p)->rta_len)))
#define RTA_PAYLOAD(p) ((p)->rta_len-sizeof(rtattr))
#define RTA_DATA(p) ((char*)(p)+sizeof(rtattr))
enum Scenario {MATCH,OTHER_IFACE,OTHER_FAMILY,NONDEFAULT,DONE,SILENT,ERROR_REPLY,WRONG_SEQ,CONTINUOUS,EINTR_STREAM,RACE,SEND_FAIL};
static Scenario scenario;static bool nonblock;static int sentFamily,closed,reads,waits;
static unsigned mockIndex=7;
unsigned fake_index(const char*) {return mockIndex;}
int fake_socket(int,int type,int) {nonblock=type&SOCK_NONBLOCK;return 11;}
int fake_close(int) {++closed;return 0;}
int fake_setsockopt(int,int,int,const void*,size_t) {errno=ENOPROTOOPT;return -1;}
ssize_t fake_sendto(int,const void *data,size_t len,int,const sockaddr*,size_t) {
    sentFamily=((const rtmsg*)NLMSG_DATA((const nlmsghdr*)data))->rtm_family;
    if(scenario==SEND_FAIL){errno=EAGAIN;return -1;}return len;
}
int fake_poll(pollfd *fd,unsigned long,int timeout) {
    ++waits;assert(timeout>0&&timeout<=1000);
    if(scenario==SILENT)return 0;
    if(scenario==EINTR_STREAM){std::this_thread::sleep_for(std::chrono::milliseconds(10));errno=EINTR;return -1;}
    fd->revents=POLLIN;return 1;
}
ssize_t fake_recv(int,void *data,size_t,int flags) {
    ++reads;
    if(!nonblock||!(flags&MSG_DONTWAIT))throw std::runtime_error("blocking receive after rejected SO_RCVTIMEO");
    if(scenario==RACE){errno=EAGAIN;std::this_thread::sleep_for(std::chrono::milliseconds(10));return -1;}
    auto *header=(nlmsghdr*)data;header->nlmsg_len=sizeof(nlmsghdr);header->nlmsg_seq=1;
    header->nlmsg_type=NLMSG_DONE;
    if(scenario==DONE || (reads>1&&scenario!=CONTINUOUS&&scenario!=MATCH))return header->nlmsg_len;
    if(scenario==ERROR_REPLY){header->nlmsg_type=NLMSG_ERROR;return header->nlmsg_len;}
    if(scenario==CONTINUOUS)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    header->nlmsg_type=RTM_NEWROUTE;header->nlmsg_len=NLMSG_LENGTH(sizeof(rtmsg))+8;
    if(scenario==WRONG_SEQ)header->nlmsg_seq=77;
    auto *route=(rtmsg*)NLMSG_DATA(header);route->rtm_family=sentFamily;route->rtm_type=RTN_UNICAST;
    if(scenario==OTHER_FAMILY)route->rtm_family=sentFamily==AF_INET6?AF_INET:AF_INET6;
    if(scenario==NONDEFAULT)route->rtm_dst_len=64;
    auto *attr=RTM_RTA(route);attr->rta_type=RTA_OIF;attr->rta_len=8;
    uint32_t index=(scenario==OTHER_IFACE||scenario==CONTINUOUS)?9:7;memcpy(RTA_DATA(attr),&index,4);
    return header->nlmsg_len;
}
#define socket fake_socket
#define close fake_close
#define sendto fake_sendto
#define recv fake_recv
#define poll fake_poll
#define setsockopt fake_setsockopt
#define if_nametoindex fake_index
#define NETMGR_EXT_LOG_W(...) ((void)0)
class NearlinkIpv6Runtime {public:static bool HasDefaultRouteOnInterface(const std::string&,int32_t);};
''' + method + r'''
int main() {
    for(int family:{AF_INET,AF_INET6}) {
        for(Scenario value:{MATCH,OTHER_IFACE,OTHER_FAMILY,NONDEFAULT,DONE,SILENT,ERROR_REPLY,WRONG_SEQ,SEND_FAIL}) {
            scenario=value;reads=waits=0;int before=closed;
            assert(NearlinkIpv6Runtime::HasDefaultRouteOnInterface("rmnet0",family)==(value==MATCH));
            assert(closed==before+1);if(value==SILENT)assert(reads==0);
        }
    }
    for(Scenario value:{CONTINUOUS,EINTR_STREAM,RACE}) {
        scenario=value;reads=waits=0;
        auto start=std::chrono::steady_clock::now();bool stopped=false;
        std::deque<std::function<void()>> queue;
        queue.push_back([] {assert(!NearlinkIpv6Runtime::HasDefaultRouteOnInterface("rmnet0",AF_INET6));});
        queue.push_back([&] {stopped=true;}); // Stop queued after the incomplete route query.
        while(!queue.empty()){auto task=queue.front();queue.pop_front();task();}
        auto elapsed=std::chrono::steady_clock::now()-start;
        assert(stopped&&elapsed<std::chrono::milliseconds(1300));
    }
    mockIndex=0;assert(!NearlinkIpv6Runtime::HasDefaultRouteOnInterface("absent",AF_INET6));
    mockIndex=7;assert(!NearlinkIpv6Runtime::HasDefaultRouteOnInterface("rmnet0",-1));
    std::cout<<"production route query: selected interface/family, malformed completion, nonblocking I/O, total deadline, queued stop PASS\n";
}
'''
with tempfile.TemporaryDirectory(prefix='p3-route-query-') as directory:
    out = Path(directory)
    (out / 'test.cpp').write_text(fixture, encoding='utf-8')
    subprocess.run(['g++', '-std=c++17', str(out / 'test.cpp'), '-o', str(out / 'test.exe')], check=True)
    subprocess.run([str(out / 'test.exe')], check=True, timeout=10)
