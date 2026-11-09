import json
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from test_backend import backend, SCRIPT
import reporting


def message(level, text):
    return json.dumps(dict(type='log_message', levelname=level, message=text))


def result(job=0, borg=0, create=0, finished=1):
    return 'M45_BACKUP_RESULT ' + json.dumps(dict(exit_code=job, borg_exit_code=borg,
                                               create_exit_code=create, finished=finished))


class ReportingTests(unittest.TestCase):
    def test_warning_and_error_messages_keep_their_operation(self):
        raw = '\n'.join(['M45_BORG_BEGIN create', message('WARNING', 'file.txt: Permission denied'),
                         'M45_BORG_RESULT create 1', 'M45_BORG_BEGIN compact',
                         message('ERROR', 'No space left'), 'M45_BORG_RESULT compact 2', result(2,2,1,0)])
        report, readable = reporting.parse(raw)
        self.assertEqual(report['outcome'], 'error')
        self.assertEqual([i['operation'] for i in report['issues']], ['create','compact'])
        self.assertIn('WARNING · Borg create: file.txt: Permission denied', readable)
        self.assertIn('ERROR · Borg compact: No space left', readable)

    def test_exit_one_is_only_a_warning_after_completed_borg_work(self):
        for summary, expected in [(result(1,1,1), 'warning'), (result(1,1,0), 'warning'),
                                  (result(1,-1,-1,0), 'error'), (result(1,1,-1,0), 'error'),
                                  (result(0,-1,-1,0), 'skipped'), (result(), 'success')]:
            with self.subTest(expected=expected, summary=summary):
                self.assertEqual(reporting.parse(summary)[0]['outcome'], expected)

    def test_truncated_or_malformed_output_never_claims_success(self):
        for raw in ['M45_BACKUP_RESULT {broken', message('WARNING','A warning without a final result'),
                    'M45_BACKUP_RESULT ' + json.dumps(dict(exit_code=True, borg_exit_code=0, create_exit_code=0, finished=1))]:
            self.assertNotEqual(reporting.parse(raw)[0]['outcome'], 'success')
        raw = 'borg: error: unrecognized arguments\n' + result(2,2,-1,0)
        self.assertIn('unrecognized arguments', reporting.parse(raw)[1])

    def test_reporting_upgrade_is_idempotent_and_preserves_other_traps(self):
        upgraded = reporting.upgrade_script(SCRIPT)
        self.assertEqual(reporting.upgrade_script(upgraded), upgraded)
        self.assertEqual(upgraded.count('# M45 Borg reporting\n'), 1)
        with self.assertRaisesRegex(ValueError, 'custom exit trap'):
            reporting.upgrade_script(SCRIPT.replace('set -euo pipefail', "set -euo pipefail\ntrap 'echo custom' EXIT"))
        with self.assertRaisesRegex(ValueError, 'custom exit trap'):
            reporting.upgrade_script(SCRIPT.replace('set -euo pipefail', "set -euo pipefail\ntrap 'echo custom' EXIT INT"))
        with self.assertRaisesRegex(ValueError, 'custom Borg wrapper'):
            reporting.upgrade_script(SCRIPT + '\nborg() { echo custom; }\n')

    def test_journal_query_is_scoped_and_older_results_are_not_reused(self):
        for invocation in ('a'*32, ''):
            with self.subTest(invocation=invocation):
                config = dict(source='/tmp', mountpoint='/not-mounted', uuid='expected')
                completed = SimpleNamespace(stdout=result(1,1,1), stderr='', returncode=0)
                with patch.object(backend,'read_config',return_value=config), \
                     patch.object(backend,'properties',side_effect=[{'InvocationID':invocation},{}]), \
                     patch.object(backend,'run',return_value='wrong'), patch.object(backend,'actual_mount',return_value=[]), \
                     patch.object(backend.subprocess,'run',return_value=completed) as journal:
                    snapshot = backend.status()
                self.assertEqual(snapshot['borg_report']['outcome'], 'warning' if invocation else 'unknown')
                if invocation:
                    self.assertIn('_SYSTEMD_INVOCATION_ID='+invocation, journal.call_args.args[0])

    def test_upgrade_reporting_uses_only_the_saved_configuration(self):
        config = {key:key for key in backend.CONFIG_FIELDS}
        config.update(warning_reporting=False, uuid='source', source_uuid='source')
        with patch.object(backend,'read_config',return_value=config), patch.object(backend,'apply') as apply, \
             patch.object(backend.os,'geteuid',return_value=0), patch.object(backend.sys,'argv',['backend','upgrade-reporting']):
            backend.main()
        self.assertEqual(apply.call_args.args[0], {key:config[key] for key in backend.CONFIG_FIELDS})


if __name__ == '__main__':
    unittest.main()
