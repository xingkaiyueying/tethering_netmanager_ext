"""Execute the production terminal IPv6 preparation with explicit kernel-call doubles."""
from pathlib import Path
import subprocess
import tempfile
repo=Path(__file__).resolve().parents[2]
source=(repo/'services/networksharemanager/src/nearlink_ipv6_runtime.cpp').read_text(encoding='utf-8')
def function(signature):
    start=source.index(signature)
    end=source.index('{',start)+1
    depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}')
        end+=1
    return source[start:end]+'\n'
with tempfile.TemporaryDirectory(prefix='shared-lla-') as directory:
    out=Path(directory)
    text=r'''
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <string>
#include <map>
#include <cassert>
#include <iostream>
#undef IFF_UP
#undef IFF_MULTICAST
constexpr const char *IFACE="sleip0";
constexpr int IFNAMSIZ=16,SOCK_CLOEXEC=0x80000,IFF_UP=1,IFF_MULTICAST=0x1000,IFF_NOARP=0x80;
constexpr int SIOCGIFFLAGS=1,SIOCSIFFLAGS=2;
struct ifreq {char ifr_name[16]{};short ifr_flags=IFF_UP;};
unsigned if_nametoindex(const char*){return 7;}
int Ioctl(int,int,ifreq*){return 0;}int Socket(int,int,int){return 1;}void Close(int){}
#define ioctl Ioctl
#define socket Socket
#define close Close
class NearlinkIpv6Runtime {public:
 std::string iface_,configuredPrefix_,layer2_,linkLocal_,added;unsigned ifindex_=0;short flags_=0;
 bool prepared_=false,flagsOwned_=false;std::map<std::string,std::string> settings;
 bool Set(const std::string&k,const std::string&v){settings[k]=v;return true;}
 bool SetToken(){return true;}bool Cleanup(){return true;}
 bool AddAddress(const std::string&a){added=a;return true;}
 bool Prepare(bool,const std::string&,const std::string& = "");
};
'''
    text+=function('bool Layer2Token(')+function('bool NearlinkIpv6Runtime::Prepare(')
    text+=r'''
int main(){
 NearlinkIpv6Runtime a,b,again,g;
 assert(a.Prepare(false,"02:11:22:33:44:55"));assert(b.Prepare(false,"02:11:22:33:44:56"));
 assert(again.Prepare(false,"02:11:22:33:44:55"));assert(g.Prepare(true,"02:aa:bb:cc:dd:ee"));
 assert(a.added!=b.added&&again.added==a.added&&a.added!="fe80::2"&&g.added=="fe80::1");
 assert(a.settings["accept_dad"]=="1"&&a.settings["dad_transmits"]=="1");
 assert(a.settings["accept_redirects"]=="0"&&a.settings["autoconf"]=="1");
 std::cout<<"shared IPv6 preparation: unique/stable member link-local, gateway router address, DAD and Redirect policy PASS\n";
}
'''
    (out/'test.cpp').write_text(text,encoding='utf-8')
    exe=out/'check.exe'
    subprocess.run(['g++','-std=c++17',str(out/'test.cpp'),'-o',str(exe),'-lws2_32'],check=True)
    subprocess.run([str(exe)],check=True)
