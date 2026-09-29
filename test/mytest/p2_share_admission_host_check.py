"""Exercise the production sharing admission ledger with failed and overlapping owners."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
header = repo / 'services/networksharemanager/include'
with tempfile.TemporaryDirectory(prefix='p2-share-admission-') as directory:
    out = Path(directory)
    (out / 'net_manager_constants.h').write_text(
        'inline constexpr int NETMANAGER_EXT_SUCCESS = 0;\n', encoding='utf-8')
    (out / 'net_manager_ext_constants.h').write_text(
        'inline constexpr int NETMANAGER_EXT_ERR_OPERATION_FAILED = -1;\n', encoding='utf-8')
    (out / 'test.cpp').write_text(r'''
#include <cassert>
#include "networkshare_admission.h"
using OHOS::NetManagerStandard::NetworkShareAdmission;
int main()
{
    auto &admission = NetworkShareAdmission::GetInstance();
    int calls = 0;
    assert(admission.LegacyResource("resource:nat", true, [&] { ++calls; return -1; }) == -1);
    assert(admission.AcquireNearlink()); // An operation that did not acquire NAT cannot leave a stale owner.
    assert(!admission.AcquireLegacy("request:wifi"));
    assert(admission.LegacyResource("resource:dns", false, [&] { ++calls; return 0; }) == 0);
    assert(calls == 1); // Late release must not touch a new NearLink gateway.
    admission.ReleaseNearlink();

    assert(admission.AcquireLegacy("request:wifi"));
    assert(admission.AcquireLegacy("request:pan"));
    assert(admission.AcquireLegacy("iface:bt-pan"));
    assert(admission.LegacyResource("resource:dns", true, [] { return 0; }) == 0);
    admission.ReleaseLegacy("request:wifi");
    admission.ReleaseLegacy("request:pan");
    assert(!admission.AcquireNearlink()); // The PAN interface and DNS remain owned.
    admission.ReleaseLegacy("iface:bt-pan");
    assert(admission.LegacyResource("resource:dns", false, [] { return -1; }) == -1);
    assert(!admission.AcquireNearlink()); // Failed cleanup keeps the known DNS owner.
    assert(admission.LegacyResource("resource:dns", false, [] { return 0; }) == 0);
    assert(admission.AcquireNearlink());
    admission.ReleaseNearlink();
}
''', encoding='utf-8')
    exe = out / 'test.exe'
    subprocess.run(['g++', '-std=c++17', '-I' + str(out), '-I' + str(header),
                    str(out / 'test.cpp'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    tracker = (repo / 'services/networksharemanager/src/networkshare_tracker.cpp').read_text(encoding='utf-8')
    method = tracker[tracker.index('int32_t NetworkShareTracker::EnableNetSharingInternal('):
                     tracker.index('int32_t NetworkShareTracker::SetWifiNetworkSharing(')]
    (out / 'rollback.cpp').write_text(r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <string>
#include "networkshare_admission.h"
#define NETMGR_EXT_LOG_I(...) ((void)0)
#define NETMGR_EXT_LOG_E(...) ((void)0)
inline constexpr int32_t NETWORKSHARE_ERROR_UNKNOWN_TYPE = -2;
namespace OHOS::NetManagerStandard {
enum class SharingIfaceType : uint32_t { SHARING_WIFI, SHARING_USB, SHARING_BLUETOOTH };
class NetsysController {
public:
    static int32_t updateResult;
    static NetsysController &GetInstance() { static NetsysController instance; return instance; }
    int32_t UpdateNetworkSharingType(uint32_t, bool) { return updateResult; }
};
int32_t NetsysController::updateResult = 0;
class NetworkShareTracker {
public:
    std::atomic<uint32_t> clientRequestsBitMask_{0};
    int32_t startResult = 0;
    int32_t stopResult = 0;
    int starts = 0;
    int stops = 0;
    int32_t SetWifiNetworkSharing(bool enable) { enable ? ++starts : ++stops; return enable ? startResult : stopResult; }
    int32_t SetUsbNetworkSharing(bool enable) { return SetWifiNetworkSharing(enable); }
    int32_t SetBluetoothNetworkSharing(bool enable) { return SetWifiNetworkSharing(enable); }
    int32_t EnableNetSharingInternal(const SharingIfaceType &type, bool enable);
};
''' + method + r'''
}
int main()
{
    using namespace OHOS::NetManagerStandard;
    auto &admission = NetworkShareAdmission::GetInstance();
    NetworkShareTracker tracker;
    auto wifi = SharingIfaceType::SHARING_WIFI;
    assert(admission.AcquireLegacy("request:0"));
    tracker.clientRequestsBitMask_ = 1;
    NetsysController::updateResult = -7;
    assert(tracker.EnableNetSharingInternal(wifi, true) == -7);
    assert(tracker.starts == 1 && tracker.stops == 1 && tracker.clientRequestsBitMask_ == 0);
    assert(admission.AcquireNearlink()); // Metadata failure with successful rollback does not strand ownership.
    admission.ReleaseNearlink();

    assert(admission.AcquireLegacy("request:0"));
    tracker.clientRequestsBitMask_ = 1;
    tracker.stopResult = -8;
    assert(tracker.EnableNetSharingInternal(wifi, true) == -7);
    assert(tracker.clientRequestsBitMask_ == 1 && !admission.AcquireNearlink());
    tracker.stopResult = 0;
    tracker.clientRequestsBitMask_ = 0;
    NetsysController::updateResult = 0;
    assert(tracker.EnableNetSharingInternal(wifi, false) == 0);
    assert(admission.AcquireNearlink());
    admission.ReleaseNearlink();

    tracker.clientRequestsBitMask_ = 1;
    tracker.startResult = -9;
    assert(tracker.EnableNetSharingInternal(wifi, true) == -9);
    assert(tracker.clientRequestsBitMask_ == 0 && admission.AcquireNearlink());
    admission.ReleaseNearlink();
}
''', encoding='utf-8')
    rollback = out / 'rollback.exe'
    subprocess.run(['g++', '-std=c++17', '-I' + str(out), '-I' + str(header),
                    str(out / 'rollback.cpp'), '-o', str(rollback)], check=True)
    subprocess.run([str(rollback)], check=True)
    print('NetworkShare admission: failed acquire, multiple owners, failed cleanup, late release, rollback PASS')
