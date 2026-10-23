"""Compile the production private UDP signal with clock/socket platform doubles."""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
source = repo / 'services/networksharemanager'
with tempfile.TemporaryDirectory(prefix='p3-upstream-signal-') as directory:
    out = Path(directory)
    def put(name, text):
        path = out / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')
    for name in ('netmgr_ext_log_wrapper.h', 'net/if.h', 'sys/random.h',
                 'sys/socket.h', 'arpa/inet.h', 'unistd.h'):
        put(name, '#pragma once\n')
    put('sys/un.h', '#pragma once\nstruct sockaddr_un {unsigned short sun_family;char sun_path[108];};\n')
    production = (source / 'src/nearlink_upstream_signal.cpp').read_text(encoding='utf-8')
    negative = os.environ.get('P3_UPSTREAM_SIGNAL_NEGATIVE')
    if negative == 'nonce':
        assert production.count('nonce == token &&') == 1
        production = production.replace('nonce == token &&', '(static_cast<void>(nonce), true) &&')
    if negative == 'sequence':
        assert production.count('revision >= receivedSequence &&') == 1
        production = production.replace('revision >= receivedSequence &&', '')
    assert production.count('using Clock = std::chrono::steady_clock;') == 1
    put('nearlink_upstream_signal.cpp', production.replace('using Clock = std::chrono::steady_clock;',
                                                          'using Clock = TestClock;'))
    put('test.cpp', Path(__file__).with_name('p3_upstream_signal_test.cpp').read_text(encoding='utf-8'))
    exe = out / 'check.exe'
    # The reused Linux Fwmark bitfields must retain their native 32-bit layout on this Windows host.
    subprocess.run(['g++', '-std=c++17', '-mno-ms-bitfields', '-Wall', '-Wextra', '-Werror', '-I' + str(out),
                    '-I' + str(source / 'include'),
                    '-I' + str(repo.parent / 'tethering_netmanager_base/interfaces/innerkits/netmanagernative/include'),
                    str(out / 'test.cpp'), '-o', str(exe), '-lws2_32'], check=True)
    subprocess.run([str(exe)], check=True)
