// The production codec/socket lifecycle runs below; the kernel and clock are explicit doubles.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <cerrno>
#include "fwmark.h"
static_assert(sizeof(OHOS::nmd::Fwmark)==sizeof(uint32_t));
using ssize_t = long long;
using socklen_t = int;
constexpr int SOCK_CLOEXEC=0x80000, SOCK_NONBLOCK=0x800, SO_BINDTODEVICE=25, MSG_DONTWAIT=0x40;
constexpr int GRND_NONBLOCK=1, IFNAMSIZ=16;
constexpr int SO_MARK=36;
struct TestClock {
    using time_point=std::chrono::steady_clock::time_point;
    inline static time_point current{std::chrono::seconds(100)};
    static time_point now(){return current;}
    static void Advance(int seconds){current+=std::chrono::seconds(seconds);}
};
struct Datagram {std::vector<uint8_t> data;sockaddr_storage source{};};
struct TestSocket {
    int node=0,family=0,netId=-1,type=0;
    std::string iface;
    sockaddr_storage local{};
    std::deque<Datagram> incoming;
};
static std::map<int,TestSocket> descriptors;
static std::array<unsigned,4> indices{10,20,11,21};
static int node=0,nextFd=100,closed=0,randomCalls=0,sendCalls=0,receiveCalls=0;
static bool dropStatus=false,dropAck=false,randomFailure=false,bindFailure=false,sendFailure=false;
static bool markFailure=false;
static std::map<int,bool> socketFailure;
static Datagram lastStatus;
static std::vector<std::string> logCalls;
unsigned fake_index(const char*){return indices[node];}
ssize_t fake_random(void *data,size_t size,unsigned flags){
    assert(flags==GRND_NONBLOCK);++randomCalls;
    if(randomFailure){errno=EAGAIN;return -1;}
    memset(data,randomCalls,size);return size;
}
int fake_socket(int family,int type,int){
    if(socketFailure[family]){errno=EMFILE;return -1;}
    int fd=nextFd++;descriptors[fd]={};auto &entry=descriptors[fd];
    entry.node=node;entry.family=family;entry.type=type;
    assert(type&SOCK_NONBLOCK);assert(type&SOCK_CLOEXEC);return fd;
}
int fake_close(int fd){assert(descriptors.erase(fd)==1);++closed;return 0;}
int fake_option(int fd,int level,int option,const void *data,int size){
    auto &entry=descriptors.at(fd);
    if(level==SOL_SOCKET && option==SO_BINDTODEVICE){entry.iface=(const char*)data;assert(size==(int)entry.iface.size()+1);}
    else if(level==SOL_SOCKET && option==SO_MARK){
        if(markFailure){errno=EACCES;return -1;}
        OHOS::nmd::Fwmark mark;memcpy(&mark.intValue,data,sizeof(mark.intValue));
        assert(size==sizeof(mark.intValue)&&mark.explicitlySelected&&mark.protectedFromVpn&&mark.uidBillingDone);
        entry.netId=mark.netId;
    } else {assert(level==IPPROTO_IPV6 && option==IPV6_V6ONLY && *(const int*)data==1);}
    return 0;
}
int fake_get_option(int,int level,int option,void *data,int *size){
    assert(level==SOL_SOCKET&&option==SO_MARK&&*size==sizeof(uint32_t));
    OHOS::nmd::Fwmark mark;mark.uidBillingDone=true;memcpy(data,&mark.intValue,sizeof(mark.intValue));return 0;
}
int fake_bind(int fd,const sockaddr *address,int size){
    if(bindFailure){errno=EACCES;return -1;}
    auto &entry=descriptors.at(fd);memcpy(&entry.local,address,size);
    if(entry.family==AF_INET){auto &a=(sockaddr_in&)entry.local;if(!a.sin_port)a.sin_port=htons(40000+fd);}
    else {auto &a=(sockaddr_in6&)entry.local;if(!a.sin6_port)a.sin6_port=htons(40000+fd);}
    assert(!entry.iface.empty() && entry.netId>=0);return 0;
}
bool same_target(const sockaddr_storage &local,const sockaddr *target){
    if(local.ss_family!=target->sa_family)return false;
    if(local.ss_family==AF_INET){auto &a=(const sockaddr_in&)local;auto &b=*(const sockaddr_in*)target;
        return a.sin_port==b.sin_port && a.sin_addr.s_addr==b.sin_addr.s_addr;}
    auto &a=(const sockaddr_in6&)local;auto &b=*(const sockaddr_in6*)target;
    return a.sin6_port==b.sin6_port && memcmp(&a.sin6_addr,&b.sin6_addr,16)==0;
}
ssize_t fake_send(int fd,const void *data,size_t size,int flags,const sockaddr *address,int){
    ++sendCalls;assert(flags&MSG_DONTWAIT);auto &from=descriptors.at(fd);
    if(sendFailure){errno=EAGAIN;return -1;}
    Datagram packet;packet.data.assign((const uint8_t*)data,(const uint8_t*)data+size);packet.source=from.local;
    if(packet.data[4]==2&&from.family==AF_INET)lastStatus=packet;
    if((dropStatus&&packet.data[4]==2)||(dropAck&&packet.data[4]==3))return size;
    for(auto &[other,to]:descriptors){
        (void)other;
        if(to.node!=(from.node^1) || !same_target(to.local,address))continue;
        auto copy=packet;
        if(to.family==AF_INET6)((sockaddr_in6&)copy.source).sin6_scope_id=indices[to.node];
        to.incoming.push_back(copy);
    }
    return size;
}
ssize_t fake_receive(int fd,void *data,size_t size,int flags,sockaddr *address,int *addressSize){
    ++receiveCalls;assert(flags&MSG_DONTWAIT);auto &entry=descriptors.at(fd);
    if(entry.incoming.empty()){errno=EAGAIN;return -1;}
    auto packet=entry.incoming.front();entry.incoming.pop_front();
    int bytes=entry.family==AF_INET?sizeof(sockaddr_in):sizeof(sockaddr_in6);
    memcpy(address,&packet.source,bytes);*addressSize=bytes;
    size_t copied=std::min(size,packet.data.size());memcpy(data,packet.data.data(),copied);return copied;
}
#define socket fake_socket
#define close fake_close
#define setsockopt fake_option
#define getsockopt fake_get_option
#define bind fake_bind
#define sendto fake_send
#define recvfrom fake_receive
#define getrandom fake_random
#define if_nametoindex fake_index
#define NETMGR_EXT_LOG_W(format,...) logCalls.emplace_back(format)
#include "nearlink_upstream_signal.cpp"
using Signal=OHOS::NetManagerStandard::NearlinkUpstreamSignal;
static Signal::Link gatewayLink{"sleip0","172.24.0.1","",10,99,true};
static Signal::Link terminalLink{"sleip0","172.24.0.3","172.24.0.1",20,108,true};
static void Gateway(Signal &g,uint8_t mask){node=0;g.Gateway(gatewayLink,mask);}
static Signal::State Terminal(Signal &t){node=1;return t.Terminal(terminalLink);}
static void Inject(const Datagram &packet){
    for(auto &[fd,socket]:descriptors){
        (void)fd;
        if(socket.node==1 && socket.family==AF_INET){socket.incoming.push_back(packet);return;}
    }
    assert(false);
}
static Datagram Future(Datagram packet){
    packet.data[5]=3;
    uint64_t sequence=0;
    for(size_t i=24;i<32;++i){sequence=(sequence<<8)|packet.data[i];}
    sequence+=1000;
    for(size_t i=0;i<8;++i){packet.data[31-i]=sequence>>(i*8);}
    return packet;
}
int main(){
    Signal gateway,terminal;
    Gateway(gateway,3);assert(!Terminal(terminal).known);Gateway(gateway,3);
    auto state=Terminal(terminal);assert(state.known&&state.available==3);Gateway(gateway,3);
    auto old=lastStatus;
    Gateway(gateway,2);state=Terminal(terminal);assert(state.known&&state.available==2);Gateway(gateway,2);
    Gateway(gateway,0);state=Terminal(terminal);assert(state.known&&state.available==0);Gateway(gateway,0);
    auto down=lastStatus;
    Inject(old);assert(Terminal(terminal).available==0); // old sequence cannot resurrect IPv4/IPv6
    auto malformed=down;malformed.data[5]=8;Inject(malformed);assert(Terminal(terminal).available==0);
    malformed=down;malformed.data[6]=1;Inject(malformed);assert(Terminal(terminal).available==0);
    malformed=Future(down);malformed.data.resize(33);Inject(malformed);assert(Terminal(terminal).available==0);
    malformed=down;malformed.data.resize(12);Inject(malformed);assert(Terminal(terminal).available==0);
    malformed=Future(down);malformed.data[8]^=1;Inject(malformed);assert(Terminal(terminal).available==0);
    malformed=down;malformed.data[5]=3;Inject(malformed);assert(Terminal(terminal).available==0);
    malformed=Future(down);((sockaddr_in&)malformed.source).sin_port=htons(9999);Inject(malformed);
    assert(Terminal(terminal).available==0);
    dropStatus=true;Gateway(gateway,1);assert(Terminal(terminal).available==0);
    dropStatus=false;TestClock::Advance(1);Gateway(gateway,1);assert(Terminal(terminal).available==1);
    dropAck=true;Gateway(gateway,3);assert(Terminal(terminal).available==3);Gateway(gateway,3);
    auto sent=sendCalls;TestClock::Advance(1);Gateway(gateway,3);assert(sendCalls>sent);
    dropAck=false;Terminal(terminal);Gateway(gateway,3);
    Gateway(gateway,2);Gateway(gateway,3);
    auto flap=Terminal(terminal);assert(flap.available==3&&flap.changed==1);
    assert(Terminal(terminal).changed==0);Gateway(gateway,3);
    sendFailure=true;Gateway(gateway,2);assert(Terminal(terminal).available==3);
    sendFailure=false;TestClock::Advance(1);Gateway(gateway,2);assert(Terminal(terminal).available==2);
    TestClock::Advance(16);assert(!Terminal(terminal).known); // lost control falls back with a new challenge
    auto nonce=randomCalls;assert(!Terminal(terminal).known);assert(randomCalls==nonce+1);
    Inject(old);assert(!Terminal(terminal).known);Gateway(gateway,0);
    assert(Terminal(terminal).known&&Terminal(terminal).available==0);Gateway(gateway,0);
    auto logs=logCalls.size();for(int i=0;i<100;++i){TestClock::Advance(1);Gateway(gateway,0);Terminal(terminal);}
    assert(logCalls.size()==logs);
    // A second peer has its own socket, token, sequence and address context.
    Signal gateway2,terminal2;
    auto gl2=gatewayLink;gl2.iface="sleip1";gl2.localIpv4="172.24.1.1";gl2.ifindex=11;
    auto tl2=terminalLink;tl2.localIpv4="172.24.1.3";tl2.gatewayIpv4="172.24.1.1";tl2.ifindex=21;
    node=2;gateway2.Gateway(gl2,2);node=3;terminal2.Terminal(tl2);node=2;gateway2.Gateway(gl2,2);
    node=3;assert(terminal2.Terminal(tl2).available==2);
    Inject(Future(lastStatus));assert(Terminal(terminal).available==0);
    gateway2.Reset();terminal2.Reset();assert(Terminal(terminal).available==0);
    // IPv6 control works without a DHCPv4 address, and IPv4-only mode needs no IPv6 socket.
    gateway.Reset();terminal.Reset();auto gl=gatewayLink;auto tl=terminalLink;
    gl.localIpv4.clear();tl.localIpv4.clear();tl.gatewayIpv4.clear();
    node=0;gateway.Gateway(gl,2);node=1;terminal.Terminal(tl);node=0;gateway.Gateway(gl,2);
    node=1;assert(terminal.Terminal(tl).available==2);
    gateway.Reset();terminal.Reset();gl=gatewayLink;tl=terminalLink;gl.ipv6=tl.ipv6=false;
    node=0;gateway.Gateway(gl,1);node=1;terminal.Terminal(tl);node=0;gateway.Gateway(gl,1);
    node=1;assert(terminal.Terminal(tl).available==1);
    // Reused ifindices, failed opens/randomness, empty queues and floods remain bounded and clean.
    ++indices[1];assert(!terminal.Terminal(tl).known);--indices[1];
    randomFailure=true;assert(!terminal.Terminal(tl).known);randomFailure=false;
    socketFailure[AF_INET]=true;assert(!terminal.Terminal(tl).known);
    socketFailure[AF_INET]=false;terminal.Reset();
    markFailure=true;assert(!terminal.Terminal(tl).known);markFailure=false;terminal.Reset();
    terminal.Reset();bindFailure=true;assert(!terminal.Terminal(tl).known);bindFailure=false;
    TestClock::Advance(5);terminal.Terminal(tl);auto before=receiveCalls;
    for(int i=0;i<100;++i){Inject(old);}
    terminal.Terminal(tl);assert(receiveCalls-before<=8);
    gateway.Reset();terminal.Reset();assert(descriptors.empty()&&closed>0);
    std::cout<<"private UDP upstream: dual/IPv4-only/IPv6-only, per-peer isolation, family loss/recovery, ACK/retry/loss, stale token/sequence/source, malformed/truncated frames, expiry, ifindex reuse, failure cleanup and bounded receive PASS\n";
    std::cout<<"steady control traffic: no packet logs PASS; boundary: socket/kernel/radio and scheduling are mocked\n";
}
