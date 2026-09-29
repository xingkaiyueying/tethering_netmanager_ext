"""Compile the real S4 JS converter against a small host NAPI substitute."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="p2-s4-napi-") as directory:
    out = Path(directory)
    (out / "napi").mkdir()
    (out / "arpa").mkdir()
    (out / "arpa/inet.h").write_text(
        '#pragma once\n#include <winsock2.h>\n#include <ws2tcpip.h>\n#undef ERROR\n'
        'extern "C" int inet_pton(int,const char*,void*);\n', encoding="utf-8")
    (out / "parcel.h").write_text(r'''
#pragma once
#include <cstdint>
#include <string>
class Parcel { public:
bool WriteBool(bool) {return true;} bool WriteInt32(int32_t) {return true;}
bool WriteUint32(uint32_t) {return true;} bool WriteUint64(uint64_t) {return true;}
bool WriteString(const std::string&) {return true;}
bool ReadBool(bool&) {return false;} bool ReadInt32(int32_t&) {return false;}
bool ReadUint32(uint32_t&) {return false;} bool ReadUint64(uint64_t&) {return false;}
bool ReadString(std::string&) {return false;}
};
class Parcelable {public: virtual ~Parcelable()=default; virtual bool Marshalling(Parcel&) const=0;};
''', encoding="utf-8")
    (out / "napi/native_api.h").write_text(r'''
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
struct Value {
 std::string text; int64_t number=0; bool flag=false;
 std::map<std::string,Value*> fields; std::vector<Value*> items;
 int type=3;
};
using napi_env=void*; using napi_value=Value*;
enum napi_status {napi_ok};
enum napi_valuetype {napi_undefined=0,napi_null=1,napi_string=2,napi_object=3};
inline napi_status napi_set_named_property(napi_env,napi_value object,const char* key,napi_value value) {
 object->fields[key]=value; return napi_ok;
}
inline napi_status napi_get_named_property(napi_env,napi_value object,const char* key,napi_value* result) {
 auto it=object->fields.find(key); *result=it==object->fields.end()?new Value:it->second;
 if(it==object->fields.end()) (*result)->type=napi_undefined;
 return napi_ok;
}
''', encoding="utf-8")
    (out / "base_context.h").write_text(r'''
#pragma once
#include <memory>
#include <napi/native_api.h>
class EventManager {};
namespace OHOS::NetManagerStandard {
class BaseContext {public:
 BaseContext(napi_env,std::shared_ptr<EventManager>&) {}
 napi_env GetEnv() const {return nullptr;}
 void SetParseOK(bool ok) {parsed=ok;}
 void SetErrorCode(int code) {error=code;}
 void SetNeedThrowException(bool yes) {throws=yes;}
 bool parsed=false,throws=false;int error=0;
};
}
''', encoding="utf-8")
    (out / "constant.h").write_text('#pragma once\nconstexpr size_t PARAM_NONE=0, PARAM_JUST_OPTIONS=1, PARAM_OPTIONS_AND_CALLBACK=2;\n', encoding="utf-8")
    (out / "net_manager_constants.h").write_text('#pragma once\nconstexpr int NETMANAGER_EXT_ERR_PARAMETER_ERROR=-1;\n', encoding="utf-8")
    (out / "napi_utils.h").write_text(r'''
#pragma once
#include <string>
#include <napi/native_api.h>
namespace OHOS::NetManagerStandard::NapiUtils {
inline napi_valuetype GetValueType(napi_env,napi_value v) {return static_cast<napi_valuetype>(v->type);}
inline std::string GetStringFromValueUtf8(napi_env,napi_value v) {return v->text;}
inline napi_value CreateObject(napi_env) {return new Value;}
inline napi_value CreateArray(napi_env,size_t count) {auto v=new Value;v->items.resize(count);return v;}
inline napi_value CreateUint32(napi_env,uint32_t n) {auto v=new Value;v->number=n;return v;}
inline napi_value CreateStringUtf8(napi_env,const std::string &s) {auto v=new Value;v->text=s;v->type=napi_string;return v;}
inline void SetArrayElement(napi_env,napi_value v,uint32_t index,napi_value item) {v->items.at(index)=item;}
inline void SetStringPropertyUtf8(napi_env,napi_value v,const std::string &key,const std::string &s) {
 v->fields[key]=CreateStringUtf8(nullptr,s);
}
inline void SetBooleanProperty(napi_env,napi_value v,const std::string &key,bool b) {
 auto x=new Value;x->flag=b;v->fields[key]=x;
}
inline void SetInt32Property(napi_env,napi_value v,const std::string &key,int32_t n) {
 v->fields[key]=CreateUint32(nullptr,static_cast<uint32_t>(n));
}
}
''', encoding="utf-8")
    (out / "test.cpp").write_text(r'''
#include <cassert>
#include <cstdio>
#include "nearlink_ipshare_converter.h"
#include "nearlink_ipshare_context.h"
#include "napi_utils.h"
using namespace OHOS::NetManagerStandard;
int main() {
 auto manager=std::make_shared<EventManager>();
 auto peer=NapiUtils::CreateStringUtf8(nullptr,"02:11:22:33:44:55");
 auto options=NapiUtils::CreateObject(nullptr);
 options->fields["mode"]=NapiUtils::CreateStringUtf8(nullptr,"DUAL_STACK");
 napi_value arguments[]={peer,options};
 NearlinkIpShareContext dual(nullptr,manager);dual.ParseParams(arguments,2);
 assert(dual.parsed&&!dual.throws&&dual.HasMode()&&dual.GetMode()==3);
 NearlinkIpShareContext legacy(nullptr,manager);legacy.ParseParams(arguments,1);
 assert(legacy.parsed&&!legacy.HasMode()&&legacy.GetMode()==1);
 options->fields["mode"]=NapiUtils::CreateStringUtf8(nullptr,"IPV6");
 NearlinkIpShareContext invalid(nullptr,manager);invalid.ParseParams(arguments,2);
 assert(!invalid.parsed&&invalid.throws);
 arguments[0]=NapiUtils::CreateStringUtf8(nullptr,"not-an-address");
 NearlinkIpShareContext badPeer(nullptr,manager);badPeer.ParseParams(arguments,1);
 assert(!badPeer.parsed&&badPeer.throws);
 NearlinkIpShareCapabilities caps;
 caps.identifierPresent=true;caps.peerCapabilityKnown=false;caps.localModes={1,3};caps.peerModes.clear();
 auto c=NearlinkIpShareConverter::CapabilitiesToJs(nullptr,caps);
 assert(c->fields.at("identifierPresent")->flag);
 assert(!c->fields.at("peerCapabilityKnown")->flag);
 assert(c->fields.at("localModes")->items.at(1)->text=="DUAL_STACK");
 assert(c->fields.at("peerModes")->items.empty());
 NearlinkIpShareStatus state;
 state.generation=UINT64_C(9007199254740993);state.sequence=42;
 state.requestedMode=3;state.selectedMode=1;state.fallbackReason="peer_rejected";
 state.ipv4.phase=2;state.ipv4.configurationAvailable=true;
 state.ipv6.phase=3;state.ipv6.validation=3;
 NearlinkIpShareAddress addr;addr.address="fd00::2";addr.prefixLength=64;
 state.ipv6.addresses.push_back(addr);
 auto v=NearlinkIpShareConverter::ToJs(nullptr,state);
 assert(v->fields.at("generation")->text=="9007199254740993");
 assert(v->fields.at("sequence")->text=="42");
 assert(v->fields.at("requestedMode")->text=="DUAL_STACK");
 assert(v->fields.at("selectedMode")->text=="IPV4");
 assert(v->fields.at("fallbackReason")->text=="peer_rejected");
 assert(v->fields.at("ipv4")->fields.at("phase")->text=="AVAILABLE");
 assert(v->fields.at("ipv6")->fields.at("validation")->text=="FAILED");
 assert(v->fields.at("ipv6")->fields.at("addresses")->items.at(0)->fields.at("prefixLength")->number==64);
 assert(v->fields.count("netId")==0);
 puts("S4 NAPI: legacy and dual mode parsing, invalid input, capabilities, families, exact uint64 strings PASS");
}
''', encoding="utf-8")
    executable = out / "test.exe"
    subprocess.run(["g++", "-std=c++17", f"-I{out}",
                    f'-I{repo / "frameworks/js/napi/sharing/include"}',
                    f'-I{repo / "interfaces/innerkits/netshareclient/include"}',
                    str(repo / "frameworks/js/napi/sharing/src/nearlink_ipshare_converter.cpp"),
                    str(repo / "frameworks/js/napi/sharing/src/nearlink_ipshare_context.cpp"),
                    str(out / "test.cpp"), "-o", str(executable), "-lws2_32"], check=True)
    subprocess.run([str(executable)], check=True)
