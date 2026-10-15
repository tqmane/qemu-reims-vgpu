#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Native Windows CPU-only test; requires an existing x86_64 WHPX build.

Run with --source <QEMU tree> --build <build directory> --output <test directory>.
The two production functions are compiled verbatim; no hypervisor is started.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import re
import subprocess


def function(source, name):
    match = re.search(r'(?m)^(?:static )?(?:bool|uint64_t) ' + name +
                      r'\([\s\S]*?^}', source.read_text())
    if match is None:
        raise ValueError(f'{name} definition not found in {source}')
    return match.group(0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'build', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    source, build, output = (p.resolve() for p in
                             (args.source, args.build, args.output))
    database = json.loads((build / 'compile_commands.json').read_text())
    row = next(r for r in database if r['file'].replace('\\', '/').endswith(
        '/target/i386/whpx/whpx-all.c'))
    old_source = (Path(row['directory']) / row['file']).resolve().parents[3]

    count = ctypes.c_int()
    parse = ctypes.windll.shell32.CommandLineToArgvW
    parse.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    parse.restype = ctypes.POINTER(ctypes.c_wchar_p)
    array = parse(row['command'], ctypes.byref(count))
    if not array:
        raise ctypes.WinError()
    tokens = [array[i] for i in range(count.value)]
    ctypes.windll.kernel32.LocalFree.argtypes = [ctypes.c_void_p]
    ctypes.windll.kernel32.LocalFree(array)
    relative_source = os.path.relpath(old_source, row['directory']).replace(
        '\\', '/')
    flags = []
    tokens = iter(tokens)
    for token in tokens:
        if token in ('-o', '-c', '-MF', '-MQ', '-MT'):
            next(tokens)
        elif token not in ('-MD', '-MMD'):
            flags.append(token.replace('\\', '/').replace(
                old_source.as_posix(), source.as_posix()).replace(
                relative_source, source.as_posix()))

    output.mkdir(parents=True, exist_ok=True)
    (output / 'whpx-topology-under-test.h').write_text(
        function(source / 'target/i386/cpu-system.c',
                 'cpu_x86_get_msr_core_thread_count') + '\n' +
        function(source / 'target/i386/whpx/whpx-all.c',
                 'whpx_access_msr') + '\n')
    binary = output / 'test-whpx-topology.exe'
    test = Path(__file__).with_name('test-whpx-topology.c').resolve()
    subprocess.run(flags + ['-Umain', '-UNDEBUG', '-fuse-ld=lld',
                            '-I' + str(output), str(test), '-o', str(binary)],
                   cwd=build, check=True)
    return subprocess.run([str(binary)], cwd=output).returncode


if __name__ == '__main__':
    raise SystemExit(main())
