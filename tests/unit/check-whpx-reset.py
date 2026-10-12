#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pure native Windows regression: actual WHPX/xstate code, mocked WHP I/O.

Requires --source QEMU_TREE --build CONFIGURED_BUILD --output TEST_DIRECTORY.
No partition or VM is created. Build flags and types come from the real build.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import re
import subprocess


def function(text, name):
    match = re.search(r'(?m)^(?:static )?(?:bool|int|void) ' + name +
                      r'\([\s\S]*?^}', text)
    if match is None:
        raise ValueError(f'Cannot find {name}')
    return match.group(0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'build', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--before', type=Path,
                        help='Negative control: use this old WHPX getter source')
    args = parser.parse_args()
    source, build, output = (p.resolve() for p in
                             (args.source, args.build, args.output))
    database = json.loads((build / 'compile_commands.json').read_text())
    row = next(r for r in database if r['file'].replace('\\', '/').endswith(
        '/target/i386/whpx/whpx-all.c'))
    old = (Path(row['directory']) / row['file']).resolve().parents[3]
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
    relative = os.path.relpath(old, build).replace('\\', '/')
    flags = []
    tokens = iter(tokens)
    for token in tokens:
        if token in ('-o', '-c', '-MF', '-MQ', '-MT'):
            next(tokens)
        elif token not in ('-MD', '-MMD'):
            flags.append(token.replace('\\', '/').replace(old.as_posix(),
                         source.as_posix()).replace(relative, source.as_posix()))
    output.mkdir(parents=True, exist_ok=True)
    cpu = (source / 'target/i386/cpu.c').read_text()
    table = re.search(r'(?m)^ExtSaveArea x86_ext_save_areas\[[\s\S]*?^};', cpu)
    whpx = (source / 'target/i386/whpx/whpx-all.c').read_text()
    generated = table.group(0) + '\n' + function(cpu, 'host_cpuid') + '\n#include "' + (
        source / 'target/i386/xsave_helper.c').as_posix() + '"\n'
    for name in ('whpx_reset_vcpu_state', 'whpx_get_xsave_state',
                 'whpx_set_xsave_state', 'whpx_is_xsave_enabled'):
        text = args.before.read_text() if args.before and name == \
            'whpx_get_xsave_state' else whpx
        generated += function(text, name) + '\n'
    condition = re.search(r'if \(\(level == WHPX_LEVEL_RESET_STATE[\s\S]*?'
                          r'whpx_set_xsave_state\(cpu\);\s*}', whpx).group(0)
    generated += ('static void test_xstate_write(CPUState *cpu, WHPXStateLevel level)'
                  '{ AccelCPUState *vcpu = cpu->accel; ' + condition + '}\n')
    (output / 'whpx-reset-under-test.h').write_text(generated)
    test = Path(__file__).with_name('test-whpx-reset.c').resolve()
    binary = output / 'test-whpx-reset.exe'
    subprocess.run(flags + ['-Umain', '-UNDEBUG', '-fuse-ld=lld', '-Werror',
                            '-I' + str(output), str(test), '-lglib-2.0',
                            '-o', str(binary)],
                   cwd=build, check=True)
    return subprocess.run([str(binary)], cwd=output).returncode


if __name__ == '__main__':
    raise SystemExit(main())
