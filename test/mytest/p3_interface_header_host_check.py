"""Compile production include ordering with modeled musl/UAPI guards.

The minimal headers reproduce the reported structure collision. They do not
replace an OHOS sysroot or prove full product compilation.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
base = repo.parent / 'tethering_netmanager_base/services/netsyscontroller/include'
assert '#include "i_netsys_controller_service.h"' in (base / 'netsys_controller.h').read_text(encoding='utf-8')
assert '#include <linux/if.h>' in (base / 'i_netsys_controller_service.h').read_text(encoding='utf-8')
before = os.environ.get('P3_INTERFACE_HEADERS_BEFORE_REF')
types = ('ifmap', 'ifreq', 'ifconf')

with tempfile.TemporaryDirectory(prefix='p3-interface-headers-') as directory:
    out = Path(directory)
    def put(name, text):
        path = out / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')

    definitions = '\n'.join('struct ' + name + ' { int value; };' for name in types)
    put('net/if.h', '#ifndef _NET_IF_H\n#define _NET_IF_H\n' + definitions + '\n#endif\n')
    put('linux/if.h', '#ifndef _LINUX_IF_H\n#define _LINUX_IF_H\n'
        '#ifndef _NET_IF_H\n' + definitions + '\n#endif\n#endif\n')
    put('netsys_controller.h', '#include "i_netsys_controller_service.h"\n')
    put('i_netsys_controller_service.h', '#include <linux/if.h>\n')

    def compile_order(headers, name):
        put(name + '.cpp', '\n'.join(headers) + '\n')
        return subprocess.run(['g++', '-std=c++17', '-fsyntax-only', '-I' + str(out),
                               str(out / (name + '.cpp'))], capture_output=True)

    checked = []
    failures = []
    negatives = 0
    for path in sorted((repo / 'services/networksharemanager/src').glob('*.cpp')):
        text = path.read_text(encoding='utf-8')
        if '#include <net/if.h>' not in text or '#include "netsys_controller.h"' not in text:
            continue
        if before:
            text = subprocess.check_output(['git', '-c', 'safe.directory=' + repo.as_posix(),
                '-C', str(repo), 'show', before + ':' + path.relative_to(repo).as_posix()]).decode()
        headers = re.findall(r'^#include [<"](?:net/if\.h|netsys_controller\.h)[>"]', text, re.M)
        if len(headers) != 2:
            continue
        result = compile_order(headers, path.stem)
        if result.returncode != 0:
            failures.append(path.name + '\n' + result.stderr.decode(errors='replace'))
            continue
        # Reversing the dependency must reproduce all three reported redefinitions.
        result = compile_order(['#include "netsys_controller.h"', '#include <net/if.h>'],
                               path.stem + '_wrong_order')
        diagnostics = result.stderr.decode(errors='replace')
        assert result.returncode != 0 and all(name in diagnostics for name in types), diagnostics
        negatives += 1
        checked.append(path.name)
    assert not failures, '\n'.join(failures)
    assert {'nearlink_peer_address.cpp', 'nearlink_ipv6_runtime.cpp',
            'nearlink_ipshare_controller.cpp', 'networkshare_tracker.cpp'} <= set(checked)
    print('interface include ordering: ' + ', '.join(checked) + '=PASS')
    print('reversed ordering: ' + str(negatives) + ' expected compiler failures=PASS')
    print('boundary: modeled musl/UAPI guards; OHOS product build remains pending')
