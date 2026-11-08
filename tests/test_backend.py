import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('backend', Path(__file__).resolve().parents[1] / 'backend/m45-drive-backuper.py')
backend = importlib.util.module_from_spec(spec)
spec.loader.exec_module(backend)

SCRIPT = '''#!/bin/bash
set -euo pipefail
uuid=43b29217-193a-4f61-99ff-01a137b0c8f2
mountpoint=/media/dist/$uuid
[[ $(findmnt -rn -M "$mountpoint" -o UUID) == "$uuid" ]] || exit 1
borg prune --keep-hourly 4 --keep-daily 7 --keep-weekly 4 --keep-monthly 6
'''


class BackendTests(unittest.TestCase):
    def setUp(self):
        self.data = dict(mountpoint='/media/dist/Backup-SSD', hours=6, hourly=4, daily=7, weekly=4, monthly=6, revision='a'*64,
                         excludes='# Keep source trees\npp:home/dist/.cache\nsh:home/dist/**/node_modules\n')

    def test_validation(self):
        self.assertEqual(backend.validate(self.data), self.data)
        for key, value in [('hours', True), ('hours', 5), ('hourly', -1), ('monthly', 366),
                           ('mountpoint', '/tmp/$(touch /tmp/no)'), ('mountpoint', '/tmp/../home'),
                           ('excludes', 'a\0b')]:
            data = dict(self.data, **{key: value})
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                backend.validate(data)
        data = dict(self.data, hourly=0, daily=0, weekly=0, monthly=0)
        with self.assertRaises(ValueError): backend.validate(data)
        with self.assertRaises(ValueError): backend.validate(dict(self.data, source='/etc'))

    def test_replacement_preserves_uuid_safety(self):
        data = dict(self.data, mountpoint='/media/dist/Backup SSD', daily=14)
        text = backend.replacement(SCRIPT, data)
        self.assertIn("mountpoint='/media/dist/Backup SSD'", text)
        self.assertIn('--keep-daily 14', text)
        self.assertIn('[[ $(findmnt -rn -M "$mountpoint" -o UUID) == "$uuid" ]] || exit 1', text)
        result = subprocess.run(['/bin/bash', '-n'], input=text, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_unknown_script_is_rejected(self):
        with self.assertRaises(ValueError): backend.replacement('echo changed', self.data)

    def test_read_existing_and_daily_config(self):
        with tempfile.TemporaryDirectory() as tmp:
            script, exclude, interval = [Path(tmp) / name for name in ('script','exclude','interval')]
            script.write_text(SCRIPT); exclude.write_text(self.data['excludes']); interval.write_text('[Timer]\nOnCalendar=\nOnCalendar=*-*-* 00/6:00:00\n')
            with patch.multiple(backend, SCRIPT=script, EXCLUDES=exclude, INTERVAL=interval):
                self.assertEqual(backend.read_config()['hours'], 6)
                self.assertEqual(backend.read_config()['mountpoint'], '/media/dist/43b29217-193a-4f61-99ff-01a137b0c8f2')
                interval.write_text('[Timer]\nOnCalendar=\nOnCalendar=daily\n')
                self.assertEqual(backend.read_config()['hours'], 24)

    def test_wrong_uuid_never_writes(self):
        with (patch.object(backend, 'read_config', return_value=dict(self.data, mountpoint='/old', uuid='expected')),
             patch.object(backend, 'run', return_value='other-uuid'), patch.object(backend, 'atomic') as write):
            with self.assertRaisesRegex(ValueError, 'UUID-verified'): backend.apply(self.data)
            write.assert_not_called()

    def test_running_backup_never_writes(self):
        with (patch.object(backend, 'read_config', return_value=dict(self.data, uuid='expected')),
             patch.object(backend, 'properties', return_value={'ActiveState':'activating'}), patch.object(backend, 'atomic') as write):
            with self.assertRaisesRegex(ValueError, 'running backup'): backend.apply(self.data)
            write.assert_not_called()

    def test_archives_require_verified_mount(self):
        with (patch.object(backend, 'read_config', return_value=dict(self.data, uuid='expected')),
             patch.object(backend, 'run', return_value='wrong') as run):
            with self.assertRaisesRegex(ValueError, 'not mounted'): backend.archives()
            self.assertEqual(run.call_count,1)

    def test_atomic_permissions_and_content(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'settings'
            backend.atomic(path, 'new data\n', 0o600)
            self.assertEqual(path.read_text(), 'new data\n')
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    def test_stale_editor_never_writes(self):
        with (patch.object(backend, 'read_config', return_value=dict(self.data, revision='b'*64)),
              patch.object(backend, 'atomic') as write):
            with self.assertRaisesRegex(ValueError, 'changed outside'): backend.apply(self.data)
            write.assert_not_called()

    def test_save_and_rollback_in_isolated_filesystem(self):
        for fail in (False, True):
            with self.subTest(fail=fail), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                script, exclude, interval = [root / name for name in ('script','exclude','interval')]
                script.write_text(SCRIPT); exclude.write_text('old exclusions'); interval.write_text('OnCalendar=hourly\n')
                script.chmod(0o755)
                original = {p: p.read_text() for p in (script,exclude,interval)}
                with patch.multiple(backend, SCRIPT=script, EXCLUDES=exclude, INTERVAL=interval, LOCK=root/'lock', BACKUPS=root/'backups'):
                    config = backend.read_config()
                    data = dict(self.data, mountpoint=config['mountpoint'], revision=config['revision'], hours=24, daily=14)
                    calls = []
                    def run(*args, **kwargs):
                        calls.append(args)
                        if fail and args == ('systemctl','daemon-reload') and len(calls) == 1:
                            raise RuntimeError('simulated reload failure')
                        return ''
                    with patch.object(backend,'properties',return_value={'ActiveState':'inactive'}), patch.object(backend,'run',side_effect=run):
                        if fail:
                            with self.assertRaisesRegex(RuntimeError,'simulated'): backend.apply(data)
                            self.assertEqual({p:p.read_text() for p in original},original)
                        else:
                            backend.apply(data)
                            self.assertEqual(backend.read_config()['daily'],14)
                            self.assertEqual(backend.read_config()['hours'],24)
                        self.assertEqual(script.stat().st_mode & 0o777,0o755)
                        backups = list((root/'backups').glob('*/script'))
                        self.assertEqual(len(backups),1)
                        self.assertEqual(backups[0].read_text(),original[script])


if __name__ == '__main__': unittest.main()
