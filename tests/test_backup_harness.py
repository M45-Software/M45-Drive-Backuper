"""A negative control: the integration check must catch the previously missed bug."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from backup_harness import Lab


class HarnessTests(unittest.TestCase):
    def test_missing_recovery_is_detected_even_when_borg_succeeds(self):
        with tempfile.TemporaryDirectory(prefix="m45 quoted ' $ lab-") as tmp:
            lab = Lab(Path(tmp))
            replacement = lab.backend.replacement
            def omit_recovery(text, data):
                return replacement(text, data).replace('backup_sources+=(/var/lib/m45-drive-backuper/recovery)\n', '')
            with patch.object(lab.backend, 'replacement', side_effect=omit_recovery):
                lab.render()
            result, archive, _ = lab.run_job('missing-recovery-negative-control')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(lab.recovery.is_file(), 'The package list must be generated before testing its omission')
            lab.borg('check', '--verify-data', lab.repository)
            with self.assertRaisesRegex(AssertionError, 'Recovery file was not included'):
                lab.recovery_manifest(archive)


if __name__ == '__main__':
    unittest.main()
