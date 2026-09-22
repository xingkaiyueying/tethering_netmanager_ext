"""Execute production per-family resolver/HTTP logic with deterministic socket and DNS doubles."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
src = repo / 'services/networksharemanager'
with tempfile.TemporaryDirectory(prefix='p2-s3-validation-') as directory:
    out = Path(directory)
    for folder in ('arpa', 'sys'):
        (out / folder).mkdir()
    for header in ('arpa/inet.h', 'sys/socket.h', 'netdb.h'):
        (out / header).write_text('#pragma once\n#include "mock.h"\n')
    (out / 'poll.h').write_text('#pragma once\n#include "mock.h"\n')
    (out / 'fcntl.h').write_text('#pragma once\n#define F_GETFL 1\n#define F_SETFL 2\n#define O_NONBLOCK 4\n')
    (out / 'unistd.h').write_text('#pragma once\n')
    (out / 'net_conn_client.h').write_text('#pragma once\n#include "mock.h"\n')
    (out / 'mock.h').write_text(r'''
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <cstring>
#include <algorithm>
#include <cassert>
constexpr int SOCK_CLOEXEC=0,MSG_NOSIGNAL=0,QEURY_TYPE_NETSYS=1;
#ifndef POLLOUT
constexpr short POLLOUT=0x0010;
#endif
struct queryparam {int qp_netid=0,qp_type=0;};
inline int activeFamily=0,failFamily=0,closed=0,bindError=0;
inline size_t offset=0;
inline std::string response="HTTP/1.1 204 No Content\r\n";
inline int Resolve(const char*,const char*,const addrinfo*h,addrinfo**out,queryparam*q){
 assert(q->qp_netid==42&&q->qp_type==QEURY_TYPE_NETSYS);activeFamily=h->ai_family;
 static addrinfo entry{};entry.ai_family=h->ai_family;*out=&entry;return 0;}
inline void Free(addrinfo*){}
inline int Socket(int,int,int){offset=0;return 3;}
inline int Connect(int,const sockaddr*,size_t){return 0;}
inline int Poll(pollfd*,int,int){return 1;}
inline int Fcntl(int,int,int){return 0;}
inline int GetOption(int,int,int,void*value,socklen_t*){*static_cast<int*>(value)=0;return 0;}
inline int SetOption(int,int,int,const void*,size_t){return 0;}
inline int Send(int,const void*,size_t size,int){return std::min<size_t>(size,7);}
inline int Recv(int,void*data,size_t size,int){
 if(activeFamily==failFamily)return 0;size_t n=std::min({size,size_t(3),response.size()-offset});
 memcpy(data,response.data()+offset,n);offset+=n;return n;}
inline int Close(int){++closed;return 0;}
namespace OHOS::NetManagerStandard {struct NetConnClient {
static NetConnClient &GetInstance(){static NetConnClient n;return n;}
int BindSocket(int,int netId){assert(netId==42);return bindError;}};}
#define getaddrinfo_ext Resolve
#define freeaddrinfo Free
#define socket Socket
#define connect Connect
#define poll Poll
#define fcntl Fcntl
#define getsockopt GetOption
#define setsockopt SetOption
#define send Send
#define recv Recv
#define close Close
''')
    production = (src / 'src/nearlink_family_validation.cpp').read_text().replace(
        '/system/etc/netdetectionurl.conf', 'config.txt')
    (out / 'test.cpp').write_text(production + r'''
#include <cstdio>
using namespace OHOS::NetManagerStandard;
int main(){
 auto r=ValidateNearlinkFamilies(42,true,true);assert(r.ipv4==0&&r.ipv6==0);
 {std::ofstream f("config.txt");f<<"HttpsProbeUrl:https://unused.example/\r\nHttpProbeUrl:http://endpoint.example/generate_204\r\n";}
 r=ValidateNearlinkFamilies(42,true,true);assert(r.ipv4==2&&r.ipv6==2&&closed==2);
 failFamily=AF_INET6;r=ValidateNearlinkFamilies(42,true,true);assert(r.ipv4==2&&r.ipv6==3);
 failFamily=0;response="HTTP/1.1 302 Found\r\n";
 r=ValidateNearlinkFamilies(42,true,false);assert(r.ipv4==3&&r.ipv6==0);
 bindError=-1;r=ValidateNearlinkFamilies(42,true,true);assert(r.ipv4==3&&r.ipv6==3);
 {std::ofstream f("config.txt");f<<"HttpProbeUrl:http://endpoint.example:65536/generate_204";}
 r=ValidateNearlinkFamilies(42,true,true);assert(r.ipv4==0&&r.ipv6==0);
 std::string host,port,path,authority;
 {std::ofstream f("config.txt");f<<"HttpProbeUrl:http://endpoint.example:8080?x=1\n";}
 assert(ReadProbe(host,port,path,authority));assert(host=="endpoint.example"&&port=="8080"&&path=="/?x=1"&&authority=="endpoint.example:8080");
 for(auto value:{"https://endpoint.example/", "http://user@endpoint.example/", "http://endpoint.example:0/", "http://endpoint.example:abc/", "http://endpoint.example/#x", "http://endpoint.example/ bad"}) {
  {std::ofstream f("config.txt");f<<"HttpProbeUrl:"<<value<<"\n";}
  assert(!ReadProbe(host,port,path,authority));
 }
 {std::ofstream f("config.txt");f<<"HttpProbeUrl:http://one.example/\nHttpProbeUrl:http://two.example/\n";}
 assert(!ReadProbe(host,port,path,authority));
 puts("family validation: netId resolver/socket binding, IPv4/IPv6 independent outcomes, fragmented HTTP 204, redirect/failure rejection, missing config UNKNOWN PASS");
}
''')
    exe = out / 'test.exe'
    subprocess.run(['g++', '-std=c++17', f'-I{out}', f'-I{src / "include"}',
                    str(out / 'test.cpp'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], cwd=out, check=True)
