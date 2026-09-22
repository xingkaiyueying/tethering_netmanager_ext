"""Execute actual status serialization with a bounded in-memory Parcel substitute."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='p2-s3-contract-') as directory:
    out = Path(directory)
    (out / 'arpa').mkdir()
    (out / 'arpa/inet.h').write_text('#pragma once\n#include <winsock2.h>\n#include <ws2tcpip.h>\n#undef ERROR\nextern "C" int inet_pton(int,const char*,void*);\nextern "C" const char* inet_ntop(int,const void*,char*,size_t);\n')
    (out / 'parcel.h').write_text(r'''
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
class Parcel { public:
std::vector<uint8_t> bytes; size_t pos=0; int fail=-1, writes=0;
template<class T> bool Put(T n) {
 if(writes++==fail)return false; auto p=reinterpret_cast<uint8_t*>(&n); bytes.insert(bytes.end(),p,p+sizeof(T));return true;
}
template<class T> bool Get(T &n) {
 if(pos>bytes.size() || bytes.size()-pos<sizeof(T))return false;memcpy(&n,bytes.data()+pos,sizeof(T));pos+=sizeof(T);return true;
}
bool WriteInt32(int32_t n){return Put(n);} bool WriteUint32(uint32_t n){return Put(n);}
bool WriteUint64(uint64_t n){return Put(n);} bool WriteBool(bool n){return Put<uint8_t>(n);}
bool WriteString(const std::string&s){if(!Put<uint32_t>(s.size()) || writes++==fail)return false;bytes.insert(bytes.end(),s.begin(),s.end());return true;}
bool ReadInt32(int32_t&n){return Get(n);} bool ReadUint32(uint32_t&n){return Get(n);}
bool ReadUint64(uint64_t&n){return Get(n);} bool ReadBool(bool&n){uint8_t v;if(!Get(v)||v>1)return false;n=v;return true;}
bool ReadString(std::string&s){uint32_t n;if(!Get(n)||n>bytes.size()-pos)return false;s.assign((char*)bytes.data()+pos,n);pos+=n;return true;}
};
class Parcelable {public:virtual ~Parcelable()=default;virtual bool Marshalling(Parcel&)const=0;};
''')
    (out / 'test.cpp').write_text(r'''
#include <cassert>
#include <cstdio>
#include "nearlink_ip_share_status.h"
using namespace OHOS::NetManagerStandard;
int main(){
 NearlinkIpShareStatus status;
 status.role=NearlinkIpShareRole::TERMINAL;status.state=NearlinkIpShareState::ACTIVE;
 status.peerAddress="02:11:22:33:44:55";status.ifaceName="sleip0";
 status.generation=UINT64_C(9007199254741001);status.sequence=47;status.requestedMode=status.selectedMode=3;
 status.ipv6.phase=2;status.ipv6.configurationAvailable=true;
 NearlinkIpShareAddress a;a.address="fd77:77:1::2";a.prefixLength=64;a.scopeId=7;a.dadState=2;
 a.preferredLifetime=60;a.validLifetime=120;status.ipv6.addresses.push_back(a);
 NearlinkIpShareRoute r;r.destination="::";r.gateway="fe80::1";r.scopeId=7;r.lifetime=30;status.ipv6.routes.push_back(r);
 NearlinkIpShareDns d;d.address="fd77:77:1::53";d.transportFamily=2;d.source=2;d.lifetime=20;status.ipv6.dns.push_back(d);
 Parcel p;assert(status.Marshalling(p));NearlinkIpShareStatus round;
 assert(round.ReadFromParcel(p)&&round.generation==status.generation&&round.ipv6.routes[0].scopeId==7);
 for(size_t i=0;i<p.bytes.size();++i){Parcel cut;cut.bytes.assign(p.bytes.begin(),p.bytes.begin()+i);
  NearlinkIpShareStatus target;target.sequence=999;assert(!target.ReadFromParcel(cut)&&target.sequence==999);}
 for(int i=0;i<p.writes;++i){Parcel fail;fail.fail=i;assert(!status.Marshalling(fail));}
 auto bad=status;bad.ipv6.routes[0].scopeId=0;Parcel q;assert(!bad.Marshalling(q));
 bad=status;bad.ipv6.addresses[0].address="192.168.77.2";assert(!bad.Marshalling(q));
 bad=status;bad.ipv6.addresses.push_back(a);assert(!bad.Marshalling(q));
 bad=status;bad.ipv6.addresses[0].preferredLifetime=121;assert(!bad.Marshalling(q));
 bad=status;bad.ipv6.externalAvailable=true;assert(!bad.Marshalling(q));
 bad=status;bad.peerAddress=std::string(18,'x');assert(!bad.Marshalling(q));
 p.pos=0;p.bytes[0]=99;assert(!round.ReadFromParcel(p));
 puts("status parcel: dual roundtrip, every truncation/write failure, atomic read, invalid family/scope/lifetime/duplicate/validation PASS");
}
''')
    exe = out / 'test.exe'
    subprocess.run(['g++', '-std=c++17', f'-I{out}',
                    f'-I{repo / "interfaces/innerkits/netshareclient/include"}',
                    str(out / 'test.cpp'), '-o', str(exe), '-lws2_32'], check=True)
    subprocess.run([str(exe)], check=True)
