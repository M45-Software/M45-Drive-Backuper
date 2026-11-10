import copy
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from test_backend import backend


class AttemptHistoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.env = patch.dict(os.environ, XDG_STATE_HOME=self.temp.name)
        self.env.start()
        self.addCleanup(self.env.stop)
        self.completed = dict(
            config=dict(uuid='disk', repository='/backup/repo', source='/home/user'),
            service=dict(ActiveState='inactive', Result='success', InvocationID='a'*32,
                         ExecMainExitTimestamp='Mon 2026-11-09 17:00:00 MST'),
            borg_report=dict(outcome='success', issues=[]), logs='Backup completed',
            activity=[], log_truncated=False)

    def rebooted(self):
        snapshot = copy.deepcopy(self.completed)
        snapshot.update(service=dict(ActiveState='inactive', Result='success', InvocationID='',
                                     ExecMainExitTimestamp='', ExecMainStartTimestamp=''),
                        borg_report=dict(outcome='unknown'), logs='', activity=[])
        return snapshot

    def test_reboot_restores_success_warning_and_failure_with_diagnostics(self):
        for outcome, result, active in [('success', 'success', 'inactive'),
                                        ('warning', 'exit-code', 'failed'),
                                        ('error', 'exit-code', 'failed')]:
            saved = copy.deepcopy(self.completed)
            saved['borg_report']['outcome'] = outcome
            saved['service'].update(Result=result, ActiveState=active)
            backend.retain_last_attempt(saved)
            boot = self.rebooted()
            backend.retain_last_attempt(boot)
            for key in ('service', 'borg_report', 'logs', 'activity'):
                self.assertEqual(boot[key], saved[key])
            self.assertTrue(boot['previous_attempt'])

    def test_new_attempt_never_inherits_old_success(self):
        backend.retain_last_attempt(self.completed)
        for changes in (dict(ActiveState='active'), dict(ActiveState='activating'),
                        dict(ActiveState='failed'), dict(InvocationID='b'*32),
                        dict(ExecMainStartTimestamp='new start')):
            current = self.rebooted()
            current['service'].update(changes)
            backend.retain_last_attempt(current)
            self.assertNotIn('previous_attempt', current)
            self.assertEqual(current['borg_report']['outcome'], 'unknown')

    def test_history_is_scoped_to_backup_plan_and_corrupt_history_is_ignored(self):
        backend.retain_last_attempt(self.completed)
        boot = self.rebooted()
        boot['config']['repository'] = '/different/repo'
        backend.retain_last_attempt(boot)
        self.assertNotIn('previous_attempt', boot)
        path = Path(self.temp.name) / 'm45-drive-backuper/last-attempt.json'
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        for text in ('{bad json', '[]', json.dumps(dict(version=1, identity=self.completed['config'], attempt=None))):
            path.write_text(text)
            boot = self.rebooted()
            backend.retain_last_attempt(boot)
            self.assertNotIn('previous_attempt', boot)
