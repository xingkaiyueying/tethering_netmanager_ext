
#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>
template<class T> class sptr : public std::shared_ptr<T> {
public:
    using std::shared_ptr<T>::shared_ptr;
    sptr(T *p) : std::shared_ptr<T>(p) {}
    template<class... A> static sptr MakeSptr(A&&... a) { return sptr(new T(std::forward<A>(a)...)); }
};
inline std::vector<std::string> calls;
inline std::map<std::string,int> errors;
inline int call(const std::string &name) { calls.push_back(name); return errors[name]; }
constexpr int EOK=0, NETSYS_SUCCESS=0, NETMANAGER_SUCCESS=0, NETMANAGER_EXT_SUCCESS=0;
constexpr int NETMANAGER_EXT_ERR_PARAMETER_ERROR=-1, NETMANAGER_EXT_ERR_OPERATION_FAILED=-2;
namespace NetsysNative { struct InterfaceConfigurationParcel { std::string ifName, ipv4Addr; }; }
constexpr int NETMANAGER_EXT_ERR_LOCAL_PTR_NULL=-3, DHCP_SUCCESS=0;
inline int strcpy_s(char *d, size_t n, const char *s) { if(strlen(s)>=n) return -1; strcpy(d,s); return 0; }
#define NETMGR_EXT_LOG_I(...) ((void)0)
#define NETMGR_EXT_LOG_E(...) ((void)0)
#include "nearlink_ip_share_status.h"
namespace OHOS::NetManagerStandard {
class INearlinkIpShareEventCallback {
public: virtual void OnNearlinkIpShareStateChanged(const NearlinkIpShareStatus &) {}
};
class NetworkShareConfiguration {
    std::string gateway="192.168.77.1", start="192.168.77.2", end="192.168.77.20";
public:
    const std::string &GetNearlinkIpv4Addr() { return gateway; }
    const std::string &GetNearlinkDhcpStart() { return start; }
    const std::string &GetNearlinkDhcpEnd() { return end; }
};
struct INetAddr {
    enum { IPV4=1, IPV6=2 };
    int type_=0, family_=0, prefixlen_=0;
    std::string address_, netMask_;
    bool operator==(const INetAddr &a) const { return type_==a.type_ && family_==a.family_ &&
        prefixlen_==a.prefixlen_ && address_==a.address_ && netMask_==a.netMask_; }
};
struct Route {
    std::string iface_;
    INetAddr destination_, gateway_;
    bool hasGateway_=true, isDefaultRoute_=false;
    bool operator==(const Route &r) const {return iface_==r.iface_ && destination_==r.destination_ && gateway_==r.gateway_ && hasGateway_==r.hasGateway_;}
};
struct NetLinkInfo {
    std::string ifaceName_;
    uint16_t mtu_=0;
    std::vector<INetAddr> netAddrList_, dnsList_;
    std::vector<Route> routeList_;
    bool isUserDefinedDnsServer_=false;
    bool operator==(const NetLinkInfo &l) const {return netAddrList_==l.netAddrList_ && routeList_==l.routeList_ && dnsList_==l.dnsList_;}
};
struct NetSupplierInfo { bool isAvailable_=false; int score_=0; };
struct NetHandle { int id=10; int GetNetId() const { return id; } };
struct NetAllCapabilities {};
class INetConnCallback { public: virtual ~INetConnCallback()=default; };
class NetConnCallbackStub : public INetConnCallback {
public:
    virtual int32_t NetAvailable(sptr<NetHandle> &) { return 0; }
    virtual int32_t NetLost(sptr<NetHandle> &) { return 0; }
    virtual int32_t NetUnavailable() { return 0; }
    virtual int32_t NetConnectionPropertiesChange(sptr<NetHandle> &, const sptr<NetLinkInfo> &) { return 0; }
};
enum NetCap { NET_CAPABILITY_INTERNET, NET_CAPABILITY_NOT_VPN };
constexpr int BEARER_BLUETOOTH=2;
class NetConnClient {
public:
    bool available=false, hasDefault=false;
    std::string iface="wlan0";
    NetLinkInfo lastLink;
    int registrations=0;
    static NetConnClient &GetInstance() { static NetConnClient n; return n; }
    int GetNetIdByIdentifier(const char*,std::list<int32_t> &ids) {ids={42}; return 0;}
    int GetDefaultNet(NetHandle &) { return hasDefault ? 0 : -1; }
    int GetConnectionProperties(const NetHandle &, NetLinkInfo &l) { l.ifaceName_=iface; return 0; }
    int RegisterNetConnCallback(sptr<INetConnCallback>) { return call("monitor-add"); }
    int UnregisterNetConnCallback(sptr<INetConnCallback>) { return call("monitor-del"); }
    int RegisterNetSupplier(int, const char *, const std::set<NetCap> &, uint32_t &id) {
        int ret=call("supplier-add"); if(!ret) { id=++registrations; available=false; } return ret;
    }
    int UpdateNetSupplierInfo(uint32_t, sptr<NetSupplierInfo> i) {
        int ret=call("supplier-available"); if(!ret) available=i->isAvailable_; return ret;
    }
    int UpdateNetLinkInfo(uint32_t, sptr<NetLinkInfo> l) {
        int ret=call("link-info"); if (!ret) lastLink=*l; return ret;
    }
    int UnregisterNetSupplier(uint32_t) { int ret=call("supplier-del"); if(!ret) available=false; return ret; }
};
class NetsysController {
public:
    static NetsysController &GetInstance() { static NetsysController n; return n; }
    int AddInterfaceAddress(const char *,const std::string &,int) { return call("address-add"); }
    int GetInterfaceConfig(NetsysNative::InterfaceConfigurationParcel &c) { c.ipv4Addr=errors["address-absent"] ? "0.0.0.0" : "192.168.77.1"; return errors["address-query"]; }
    int DelInterfaceAddress(const char *,const std::string &,int) { return call("address-del"); }
    int NetworkAddInterface(int id,const char *) { assert(id==99); return call("local-add"); }
    int NetworkRemoveInterface(int,const char *) { return call("local-del"); }
    int NetworkAddRoute(int id,const char *,const char *dest,const char *via) {
        assert(id==99 && std::string(dest)=="192.168.77.0/24" && std::string(via)=="0.0.0.0");
        return call("route-add");
    }
    int NetworkRemoveRoute(int,const char *,const char *,const char *) { return call("route-del"); }
    int StartDnsProxyListen() { return call("dns-start"); }
    int StopDnsProxyListen() { return call("dns-stop"); }
    int ShareDnsSet(int) { return call("dns-set"); }
    int IpEnableForwarding(const char *) { return call("forward-add"); }
    int IpDisableForwarding(const char *) { return call("forward-del"); }
    int IpfwdAddInterfaceForward(const char *,const std::string &) { return call("iface-forward-add"); }
    int IpfwdRemoveInterfaceForward(const char *,const std::string &) { return call("iface-forward-del"); }
    int EnableNat(const char *,const std::string &) { return call("nat-add"); }
    int DisableNat(const char *,const std::string &) { return call("nat-del"); }
};
class NetworkShareTracker {
public:
    std::deque<std::function<void()>> tasks;
    std::vector<NearlinkIpShareStatus> events;
    static NetworkShareTracker &GetInstance() { static NetworkShareTracker n; return n; }
    std::deque<std::function<void()>> delayed;
    bool SubmitNearlinkTask(const std::function<void()> &f, uint64_t delay=0) {
        (delay ? delayed : tasks).push_back(f); return true;
    }
    void SendNearlinkStateChange(const NearlinkIpShareStatus &s) { events.push_back(s); }
    void Drain() { while(!tasks.empty()) { auto f=tasks.front(); tasks.pop_front(); f(); } }
};
}
namespace OHOS::Nearlink {
using NearlinkIpShareRole = OHOS::NetManagerStandard::NearlinkIpShareRole;
using NearlinkIpShareState = OHOS::NetManagerStandard::NearlinkIpShareState;
class NearlinkIpShareStatus { public:
    NearlinkIpShareRole role{NearlinkIpShareRole::NONE};
    NearlinkIpShareState state{NearlinkIpShareState::IDLE};
    std::string peerAddress, ifaceName, contextId, errorStage;
    int32_t errorCode=0, selectedMode=0;
    uint64_t generation=0, sequence=0;
    bool serviceReady=false;
};
enum class NearlinkIpShareMode { NONE=0, IPV4=1, DUAL_STACK=3 };
using NearlinkIpShareCapabilities=OHOS::NetManagerStandard::NearlinkIpShareCapabilities;
struct NearlinkIpShareAddressEvidence {
    uint64_t generation=0, sequence=0;
    std::string address;
    uint32_t ifindex=0, prefixLength=0, flags=0, preferredLifetime=0, validLifetime=0;
};
class NearlinkIpShareObserver {
public: virtual ~NearlinkIpShareObserver()=default;
    virtual void OnStatusChanged(const NearlinkIpShareStatus &)=0;
};
class NearlinkIpShareClient {
public:
    static NearlinkIpShareClient &GetInstance() { static NearlinkIpShareClient c; return c; }
    NearlinkIpShareStatus snapshot;
    std::vector<NearlinkIpShareAddressEvidence> evidence;
    int UpdateValidatedAddress(const NearlinkIpShareAddressEvidence &e) { evidence.push_back(e); return call("evidence"); }
    int GetStatus(NearlinkIpShareStatus &s) { s=snapshot; return call("snapshot"); }
    int QueryNearlinkIpShareCapabilities(const std::string &, NearlinkIpShareCapabilities &) { return 0; }
    int StartNearlinkGatewayWithMode(const std::string &peer, int) { return StartGateway(peer); }
    int StartNearlinkTerminalWithMode(const std::string &peer, int) { return StartTerminal(peer); }
    int observerRegistrations=0;
    bool serverHasObserver=false;
    int RegisterObserver(std::shared_ptr<NearlinkIpShareObserver>) {
        ++observerRegistrations; serverHasObserver=true; return 0;
    }
    int UnregisterObserver() { return 0; }
    int StartGateway(const std::string &) { return call("nearlink-start"); }
    int StartTerminal(const std::string &) { return call("nearlink-start"); }
    int Stop() { return call("nearlink-stop"); }
    int IsPeerSupported(const std::string &,bool &b) { b=true; return 0; }
};
class NearlinkHost {
public:
    static NearlinkHost &GetInstance() { static NearlinkHost h; return h; }
    int GetLocalAddress(std::string &s) { s="02:11:22:33:44:55"; return call("local-identity"); }
};
}
