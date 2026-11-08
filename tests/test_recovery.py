from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'backend'))
import recovery


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.manifest = dict(schema=recovery.SCHEMA, os={'ID':'ubuntu','VERSION_ID':'24.04'},
                             architecture='amd64', apt_manual=['bash','libc6:amd64'])

    def command(self,args):
        if args == ['dpkg','--print-architecture']: return 'amd64'
        return 'Inst bash [old] (new Ubuntu)\nConf bash (new Ubuntu)'

    def test_preview_never_installs_and_passes_packages_as_arguments(self):
        with patch.object(recovery,'os_release',return_value=self.manifest['os']), patch.object(recovery,'command',side_effect=self.command) as run, patch.object(recovery.subprocess,'run') as install:
            result=recovery.restore(self.manifest)
            self.assertEqual(result['package_count'],2)
            self.assertEqual(run.call_args.args[0],['apt-get','--simulate','--no-remove','install','--','bash','libc6:amd64'])
            install.assert_not_called()

    def test_rejects_commands_other_releases_architectures_and_empty_lists(self):
        invalid=[dict(apt_manual=['--allow-unauthenticated']),dict(apt_manual=['bash;touch /tmp/no']),dict(apt_manual=['bash\nsh']),dict(apt_manual=[]),dict(architecture='arm64'),dict(os={'ID':'ubuntu','VERSION_ID':'26.04'}),dict(schema='untrusted')]
        for values in invalid:
            with self.subTest(values=values), patch.object(recovery,'os_release',return_value=self.manifest['os']), patch.object(recovery,'command',side_effect=self.command), patch.object(recovery.subprocess,'run') as install:
                with self.assertRaises(ValueError): recovery.restore(dict(self.manifest,**values),apply=True)
                install.assert_not_called()

    def test_reinstall_refuses_a_plan_that_removes_software(self):
        with patch.object(recovery,'os_release',return_value=self.manifest['os']), patch.object(recovery,'command',side_effect=['amd64','Remv important-package']), patch.object(recovery.subprocess,'run') as install:
            with self.assertRaisesRegex(ValueError,'remove installed software'): recovery.restore(self.manifest,apply=True)
            install.assert_not_called()

    def test_install_rechecks_plan_and_reports_partial_failure(self):
        with patch.object(recovery,'os_release',return_value=self.manifest['os']), patch.object(recovery,'command',side_effect=self.command) as simulate, patch.object(recovery.os,'geteuid',return_value=0), patch.object(recovery.subprocess,'run') as install:
            install.return_value.returncode=1; install.return_value.stdout='Package configured'; install.return_value.stderr='A package failed'
            with self.assertRaisesRegex(ValueError,'Some packages may already have changed'): recovery.restore(self.manifest,apply=True)
            self.assertIn('--simulate',simulate.call_args.args[0])
            self.assertIn('--no-remove',install.call_args.args[0])

    def test_inventory_filters_removed_packages_and_records_incomplete_optional_data(self):
        def capture(args):
            if args[0] == 'dpkg-query': return 'bash\t5.2\tinstalled\nold\t1\tconfig-files'
            if args == ['apt-mark','showmanual']: return 'bash\nold'
            if args == ['apt-mark','showhold']: return 'bash'
            if args[0] == 'dpkg': return 'amd64'
            raise ValueError('optional inventory unavailable')
        with patch.object(recovery,'command',side_effect=capture), patch.object(recovery.shutil,'which',return_value='/usr/bin/tool'):
            result=recovery.inventory('/does-not-exist')
        self.assertEqual(result['apt_manual'],['bash'])
        self.assertEqual(result['apt_installed'],[dict(name='bash',version='5.2')])
        self.assertTrue(result['warnings'])


if __name__ == '__main__': unittest.main()
