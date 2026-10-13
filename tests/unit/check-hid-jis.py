#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile the real HID decoder and table with native build flags; no VM/GPU."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'build', 'output'):
        p.add_argument('--' + name, required=True, type=Path)
    p.add_argument('--before', type=Path)
    a = p.parse_args()
    source, build, output = (x.resolve() for x in (a.source, a.build, a.output))
    # A one-byte 0xff logical maximum is negative to signed HID parsers.
    # Decode the real descriptor so JIS usages above 0x7f are in its range.
    usb = (source / 'hw/usb/dev-hid.c').read_text()
    raw = re.search(r'qemu_keyboard_hid_report_descriptor\[\] = \{(.*?)\};', usb, re.S).group(1)
    raw = re.sub(r'/\*.*?\*/', '', raw, flags=re.S)
    descriptor = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]+)', raw))
    pos, globals_, arrays = 0, {}, []
    while pos < len(descriptor):
        prefix = descriptor[pos]
        size = (0, 1, 2, 4)[prefix & 3]
        payload = descriptor[pos + 1:pos + 1 + size]
        tag, kind = prefix >> 4, (prefix >> 2) & 3
        if kind == 1:
            globals_[tag] = int.from_bytes(payload, 'little', signed=tag in (1, 2))
        elif kind == 0 and tag == 8 and globals_.get(0) == 7 and globals_.get(7) == 8:
            arrays.append((globals_[1], globals_[2], globals_[9]))
        pos += size + 1
    assert (0, 255, 6) in arrays, arrays
    for name in ('desc_iface_keyboard', 'desc_iface_keyboard2'):
        block = re.search(r'static const USBDescIface ' + name + r' = \{(.*?)\n};', usb, re.S).group(1)
        length = re.search(r'(0x[0-9a-f]+), 0,\s*/\*  u16 len \*/', block).group(1)
        assert int(length, 16) == len(descriptor), name
    db = json.loads((build / 'compile_commands.json').read_text())
    row = next(r for r in db if r['file'].replace('\\', '/').endswith('/hw/input/hid.c'))
    old = (Path(row['directory']) / row['file']).resolve().parents[2]
    count = ctypes.c_int()
    parse = ctypes.windll.shell32.CommandLineToArgvW
    parse.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    parse.restype = ctypes.POINTER(ctypes.c_wchar_p)
    v = parse(row['command'], ctypes.byref(count))
    tokens = [v[i] for i in range(count.value)]
    ctypes.windll.kernel32.LocalFree.argtypes = [ctypes.c_void_p]
    ctypes.windll.kernel32.LocalFree(v)
    relative = os.path.relpath(old, build).replace('\\', '/')
    flags, tokens = [], iter(tokens)
    for t in tokens:
        if t in ('-o', '-c', '-MF', '-MQ', '-MT'):
            next(tokens)
        elif t not in ('-MD', '-MMD'):
            flags.append(t.replace('\\', '/').replace(old.as_posix(), source.as_posix()).replace(relative, source.as_posix()))
    text = (a.before or source / 'hw/input/hid.c').read_text()
    table = re.search(r'static const uint8_t hid_usage_keys\[[\s\S]*?\n};', text)
    assert table, 'Production HID table not found'
    generated = table.group(0) + '\n'
    for name in ('hid_keyboard_process_keycode', 'hid_keyboard_poll'):
        f = re.search(r'(?m)^(?:static )?(?:void|int) ' + name + r'\([\s\S]*?^}', text)
        assert f, name
        generated += f.group(0) + '\n'
    output.mkdir(parents=True, exist_ok=True)
    (output / 'hid-jis-under-test.h').write_text(generated)
    exe = output / 'test-hid-jis.exe'
    subprocess.run(flags + ['-Umain', '-UNDEBUG', '-fuse-ld=lld', '-Werror', '-I' + str(output),
                           str(Path(__file__).with_name('test-hid-jis.c').resolve()),
                           '-lglib-2.0', '-o', str(exe)], cwd=build, check=True)
    return subprocess.run([str(exe)], cwd=output).returncode


if __name__ == '__main__':
    raise SystemExit(main())
