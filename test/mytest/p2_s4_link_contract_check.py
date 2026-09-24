"""Check private NAPI client calls against the native library export map."""
from pathlib import Path
import re

repo = Path(__file__).resolve().parents[2]
calls = (repo / "frameworks/js/napi/sharing/src/nearlink_ipshare_async_work.cpp").read_text(encoding="utf-8")
native = (repo / "frameworks/native/netshareclient/src/networkshare_client.cpp").read_text(encoding="utf-8")
exports = (repo / "interfaces/innerkits/netshareclient/libnetshare_kits.map").read_text(encoding="utf-8")
methods = set(re.findall(r"client->([A-Z][A-Za-z0-9_]*)\s*\(", calls))
assert methods, "no private NAPI client calls found"
for method in sorted(methods):
    symbol = f"NetworkShareClient::{method}("
    assert symbol in native, f"{method}: missing native implementation"
    assert exports.count(symbol) == 1, f"{method}: missing or duplicate version-map export"

query_symbol = ("NetworkShareClient::QueryNearlinkIpShareCapabilities("
                "std::__h::basic_string<char, std::__h::char_traits<char>, "
                "std::__h::allocator<char>> const&, "
                "OHOS::NetManagerStandard::NearlinkIpShareCapabilities&)")
assert query_symbol in exports, "capability query ABI does not match the OHOS linker symbol"
print(f"S4 native export map: {len(methods)} private NAPI client methods and capability ABI PASS")
