#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
import json
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest
from unittest.mock import patch


@unittest.skipUnless(os.name == 'nt', 'Windows symlink fallback')
class InstallTreeTest(unittest.TestCase):
    script = Path(__file__).resolve().parents[2] / 'scripts/symlink-install-tree.py'

    def run_script(self, source, winerror):
        failure = OSError('symlink refused')
        failure.winerror = winerror
        output = subprocess.CompletedProcess([], 0,
            stdout=json.dumps({str(source): str(source)}).encode())
        with patch.dict(os.environ, {'MESONINTROSPECT': 'meson introspect'}), \
                patch('subprocess.run', return_value=output), \
                patch('os.symlink', side_effect=failure):
            runpy.run_path(str(self.script))

    def test_copy_existing_and_skip_unbuilt(self):
        previous = Path.cwd()
        with tempfile.TemporaryDirectory() as directory:
            try:
                os.chdir(directory)
                source = Path(directory) / 'ReimsVgpu.rom'
                source.write_bytes(b'our option ROM')
                self.run_script(source, 1314)
                copies = list(Path('qemu-bundle').rglob(source.name))
                self.assertEqual(len(copies), 1)
                self.assertEqual(copies[0].read_bytes(), source.read_bytes())
                missing = Path(directory) / 'not-built.exe'
                self.run_script(missing, 1314)
                self.assertFalse(list(Path('qemu-bundle').rglob(missing.name)))
            finally:
                os.chdir(previous)

    def test_other_errors_propagate(self):
        previous = Path.cwd()
        with tempfile.TemporaryDirectory() as directory:
            try:
                os.chdir(directory)
                with self.assertRaises(OSError) as caught:
                    self.run_script(Path(directory) / 'missing', 5)
                self.assertEqual(caught.exception.winerror, 5)
            finally:
                os.chdir(previous)


if __name__ == '__main__':
    unittest.main()
