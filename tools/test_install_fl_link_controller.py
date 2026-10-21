"""Code-only tests: controller deployment never opens or changes FL settings."""

import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import tempfile
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    'cue_controller_installer', Path(__file__).with_name('install_fl_link_controller.py'))
installer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(installer)


class ControllerInstallerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='cue-controller-install-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.hardware = self.root / 'FL user data/Settings/Hardware'
        self.backups = self.root / 'CUE app support/Controller Backups'
        self.source = self.root / installer.SCRIPT_NAME
        self.bytes = (b'# name=CUE SAMPLER Link\n# supportedDevices=CUE SAMPLER Link\n'
                      b'def OnIdle():\n    pass\n')
        self.source.write_bytes(self.bytes)
        self.destination = self.hardware / installer.CONTROLLER_NAME / installer.SCRIPT_NAME

    def install(self):
        return installer.install_controller(self.source, self.hardware, self.backups)

    def test_first_install_and_idempotent_repeat(self):
        first = self.install()
        self.assertEqual(first['status'], 'installed')
        self.assertEqual(first['destination'], str(self.destination))
        self.assertEqual(first['sha256'], hashlib.sha256(self.bytes).hexdigest())
        self.assertIsNone(first['backup'])
        self.assertEqual(self.destination.read_bytes(), self.bytes)
        before = self.destination.stat()
        repeat = self.install()
        after = self.destination.stat()
        self.assertEqual(repeat['status'], 'unchanged')
        self.assertEqual(before.st_ino, after.st_ino)
        self.assertEqual(before.st_mtime_ns, after.st_mtime_ns)
        self.assertFalse(self.backups.exists())
        self.assertEqual(list(self.destination.parent.iterdir()), [self.destination])

    def test_update_backs_up_only_own_file_and_preserves_everything_else(self):
        self.destination.parent.mkdir(parents=True)
        previous = b'# An older CUE controller implementation\n'
        self.destination.write_bytes(previous)
        self.destination.chmod(0o640)
        previous_mode = self.destination.stat().st_mode
        sibling = self.destination.parent / 'personal_mapping.py'
        sibling.write_bytes(b'# Keep this custom mapping\n')
        other_controller = self.hardware / 'User Controller/device_Custom.py'
        other_controller.parent.mkdir()
        other_controller.write_bytes(b'# Unrelated hardware script\n')
        settings = self.hardware.parent / 'FL preferences.ini'
        settings.write_bytes(b'[MIDI]\nUserRouting=keep\n')
        result = self.install()
        backup = Path(result['backup'])
        self.assertEqual(result['status'], 'updated')
        self.assertEqual(backup.read_bytes(), previous)
        if os.name == 'nt':
            # Windows chmod exposes the readonly flag, not POSIX permission bits.
            self.assertEqual(backup.stat().st_mode & stat.S_IWRITE,
                             previous_mode & stat.S_IWRITE)
        else:
            self.assertEqual(stat.S_IMODE(backup.stat().st_mode), 0o640)
        self.assertTrue(backup.is_relative_to(self.backups))
        self.assertFalse(backup.is_relative_to(self.hardware))
        self.assertEqual(self.destination.read_bytes(), self.bytes)
        self.assertEqual(sibling.read_bytes(), b'# Keep this custom mapping\n')
        self.assertEqual(other_controller.read_bytes(), b'# Unrelated hardware script\n')
        self.assertEqual(settings.read_bytes(), b'[MIDI]\nUserRouting=keep\n')
        self.install()
        self.assertEqual(len(list(self.backups.rglob(installer.SCRIPT_NAME))), 1)

    def test_atomic_replace_failure_preserves_old_file_and_removes_staging_file(self):
        self.install()
        previous = self.destination.read_bytes()
        self.source.write_bytes(self.bytes + b'\n# new version\n')
        actual_replace = installer.os.replace

        def fail_destination(source, destination):
            if Path(destination) == self.destination:
                raise PermissionError('simulated destination failure')
            return actual_replace(source, destination)

        with mock.patch.object(installer.os, 'replace', side_effect=fail_destination):
            with self.assertRaises(PermissionError):
                self.install()
        self.assertEqual(self.destination.read_bytes(), previous)
        self.assertEqual(list(self.destination.parent.iterdir()), [self.destination])
        self.assertEqual(len(list(self.backups.rglob(installer.SCRIPT_NAME))), 1)

    def test_rejects_backups_in_scan_root_before_any_install(self):
        with self.assertRaisesRegex(ValueError, 'outside FL Hardware'):
            installer.install_controller(self.source, self.hardware, self.hardware / 'Backups')
        self.assertFalse(self.hardware.exists())

    def test_rejects_symlink_destination_without_changing_target(self):
        unrelated = self.root / 'unrelated.py'
        unrelated.write_bytes(b'# Keep this external file\n')
        self.destination.parent.mkdir(parents=True)
        try:
            self.destination.symlink_to(unrelated)
        except OSError as error:
            if os.name == 'nt' and getattr(error, 'winerror', None) == 1314:
                self.skipTest('Windows symlink privilege is unavailable')
            raise
        with self.assertRaisesRegex(ValueError, 'symlink'):
            self.install()
        self.assertTrue(self.destination.is_symlink())
        self.assertEqual(unrelated.read_bytes(), b'# Keep this external file\n')
        self.assertFalse(self.backups.exists())

    def test_rejects_directory_destination(self):
        self.destination.mkdir(parents=True)
        with self.assertRaisesRegex(ValueError, 'regular file'):
            self.install()
        self.assertFalse(self.backups.exists())

    def test_rejects_missing_or_invalid_source_without_creating_destination(self):
        self.source.unlink()
        with self.assertRaises(FileNotFoundError):
            self.install()
        self.source.write_text('# A different controller\n', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'metadata'):
            self.install()
        self.source.write_bytes(self.bytes + b'def bad syntax\n')
        with self.assertRaises(SyntaxError):
            self.install()
        self.assertFalse(self.hardware.exists())

    def test_cli_uses_explicit_roots_and_reports_verified_manifest(self):
        output = io.StringIO()
        with mock.patch.object(installer, 'DEFAULT_SOURCE', self.source):
            with contextlib.redirect_stdout(output):
                status = installer.main(['--hardware-root', str(self.hardware),
                                         '--backup-root', str(self.backups)])
        self.assertEqual(status, 0)
        self.assertEqual(json.loads(output.getvalue())['sha256'], hashlib.sha256(self.bytes).hexdigest())
        self.assertEqual(self.destination.read_bytes(), self.bytes)

    def test_cli_reports_failure_without_success_manifest(self):
        with mock.patch.object(installer, 'DEFAULT_SOURCE', self.root / 'missing.py'):
            with contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as failure:
                    installer.main(['--hardware-root', str(self.hardware),
                                    '--backup-root', str(self.backups)])
        self.assertEqual(failure.exception.code, 1)
        self.assertFalse(self.hardware.exists())


if __name__ == '__main__':
    unittest.main()
