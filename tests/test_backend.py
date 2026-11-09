import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import shlex
import tempfile
import unittest
import sys
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1] / 'backend'))
spec = importlib.util.spec_from_file_location('backend', Path(__file__).resolve().parents[1] / 'backend/m45-drive-backuper.py')
backend = importlib.util.module_from_spec(spec)
spec.loader.exec_module(backend)

SCRIPT = '''#!/bin/bash
set -euo pipefail
uuid=43b29217-193a-4f61-99ff-01a137b0c8f2
mountpoint=/media/dist/$uuid
export BORG_REPO="$mountpoint/borg-home-example"
[[ $(findmnt -rn -M "$mountpoint" -o UUID) == "$uuid" ]] || exit 1
borg prune --keep-hourly 4 --keep-daily 7 --keep-weekly 4 --keep-monthly 6
# Reclaim abandoned transaction data before pruning needs space for metadata.
borg compact --verbose
(( available > 10737418240 )) || { echo 'Less than 10 GiB free after cleanup; backup stopped.' >&2; exit 1; }
backup_rc=0
borg create --stats --show-rc --compression zstd,3 --one-file-system \\
    --exclude-from /etc/local-system-backup.exclude --exclude-caches \\
    '::home-{now:%Y-%m-%dT%H:%M:%S}' /home/dist || backup_rc=$?
exit "$backup_rc"
'''


class BackendTests(unittest.TestCase):
    def setUp(self):
        self.data = dict(mountpoint='/media/dist/Backup-SSD', hours=6, hourly=4, daily=7, weekly=4, monthly=6, revision='a'*64,
                         source='/tmp', source_kind='folder', destination_kind='folder', compression='zstd,3', scan_mode='incremental', reserve_gib=10, quota_gib=-1, exclude_caches=True, auto_mount=False, repository='', include_system=False, package_recovery=False,
                         excludes='# Keep source trees\npp:home/dist/.cache\nsh:home/dist/**/node_modules\n')

    def test_validation(self):
        self.assertEqual(backend.validate(self.data), self.data)
        for key, value in [('hours', True), ('hours', 5), ('hourly', -1), ('monthly', 366),
                           ('mountpoint', '/tmp/bad\nmount'), ('mountpoint', '/tmp/../home'),
                           ('excludes', 'a\0b'), ('compression','zstd,3;touch /tmp/no'), ('scan_mode','image'),
                           ('source','relative'), ('source','/tmp\ncommand'), ('reserve_gib',0), ('quota_gib',True), ('exclude_caches',1), ('auto_mount',1)]:
            data = dict(self.data, **{key: value})
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                backend.validate(data)
        data = dict(self.data, hourly=0, daily=0, weekly=0, monthly=0)
        with self.assertRaises(ValueError): backend.validate(data)
        with self.assertRaises(ValueError): backend.validate(dict(self.data, command='arbitrary'))

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

    def test_system_sources_are_archived_and_recovery_refresh_precedes_cleanup(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); source=root/'home'; settings=root/'settings'; source.mkdir(); settings.mkdir()
            (source/'personal.txt').write_text('personal'); (settings/'service.conf').write_text('settings')
            data=dict(self.data,source=str(source),include_system=True,package_recovery=True)
            with patch.object(backend.recovery,'SYSTEM_PATHS',[str(settings),str(root/'missing')]):
                text=backend.replacement(backend.replacement(SCRIPT,data),data)
            self.assertEqual(text.count('# M45 include system settings: yes'),1)
            self.assertEqual(text.count('backend recovery-refresh'),1)
            self.assertLess(text.index('backend recovery-refresh'),text.index('borg compact'))
            self.assertIn('"${backup_sources[@]}"',text)
            checks=text.split('# M45 source checks\n')[1].split('# End M45 source checks')[0]
            result=subprocess.run(['/bin/bash','-c','source='+shlex.quote(str(source))+'\n'+checks+'\nprintf "%s\\n" "${backup_sources[@]}"'],check=True,text=True,capture_output=True)
            paths=result.stdout.splitlines(); self.assertEqual(paths,[str(source),str(settings)])
            repo=root/'repo'; env=dict(os.environ,BORG_CACHE_DIR=str(root/'cache'),BORG_SECURITY_DIR=str(root/'security'))
            subprocess.run(['borg','init','--encryption=none',str(repo)],env=env,check=True,capture_output=True)
            subprocess.run(['borg','create',str(repo)+'::coverage',*paths],env=env,check=True,capture_output=True)
            contents=subprocess.run(['borg','list','--format','{path}{NL}',str(repo)+'::coverage'],env=env,check=True,capture_output=True,text=True).stdout
            self.assertIn('personal.txt',contents); self.assertIn('service.conf',contents)

    def test_disconnected_destination_does_not_create_repository_on_parent_filesystem(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); source = root/'source'; source.mkdir()
            destination = root/'disconnected-drive'/'backup'
            data = dict(self.data, repository=str(destination), destination_kind='drive', mountpoint=str(destination.parent))
            with patch.object(backend,'run',return_value=json.dumps({'filesystems':[{'target':str(root),'uuid':'abc-123'}]})) as run:
                with self.assertRaisesRegex(ValueError,'no longer available'): backend.configure_destination(data,{},source)
                self.assertFalse(destination.exists())
                self.assertEqual(run.call_count,1)
                data['destination_kind'] = 'folder'
                with self.assertRaisesRegex(ValueError,'expected backup drive is unavailable'):
                    backend.configure_destination(data,{'mountpoint':str(destination.parent),'uuid':'def-456'},source)
                self.assertFalse(destination.exists())

    def test_backup_options_roundtrip_and_shell_quoting(self):
        with tempfile.TemporaryDirectory() as tmp:
            script, exclude, interval = [Path(tmp) / name for name in ('script','exclude','interval')]
            data = dict(self.data, source="/tmp/folder with 'quotes' and $(commands)", scan_mode='full_scan',compression='zstd,9',reserve_gib=25,quota_gib=100,exclude_caches=False)
            text = backend.replacement(SCRIPT,data)
            script.write_text(text); exclude.write_text(''); interval.write_text('OnCalendar=hourly\n')
            with patch.multiple(backend,SCRIPT=script,EXCLUDES=exclude,INTERVAL=interval):
                config = backend.read_config()
                for key in ('source','source_kind','compression','scan_mode','reserve_gib','quota_gib','exclude_caches'):
                    self.assertEqual(config[key],data[key])
                self.assertIn('storage_quota 107374182400',text)
                self.assertNotIn('--exclude-caches',text)
                self.assertEqual(subprocess.run(['/bin/bash','-n'],input=text,text=True,capture_output=True).returncode,0)
                text = backend.replacement(text,dict(data,quota_gib=0,scan_mode='incremental',exclude_caches=True))
                self.assertIn('borg config --delete',text)
                self.assertNotIn('--files-cache disabled',text)
                self.assertEqual(text.count('# M45 source checks'),1)
                self.assertEqual(text.count('# M45 storage limit GiB:'),1)

    def test_source_drive_guard_survives_repeated_saves(self):
        data = dict(self.data,source_kind='drive',source_uuid='source-uuid')
        text = backend.replacement(backend.replacement(SCRIPT,data),data)
        self.assertEqual(text.count('source_uuid=source-uuid'),1)
        self.assertIn('[[ $(findmnt -rn -M "$source_mountpoint" -o UUID) == "$source_uuid" ]]',text)
        self.assertLess(text.index('# M45 source checks'),text.index('borg prune'))

    def test_real_borg_storage_limit_and_full_scan(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); source = root/'source'; source.mkdir(); (source/'example.txt').write_text('backup example\n')
            repo = root/'repo'
            env = dict(os.environ, BORG_CACHE_DIR=str(root/'cache'),BORG_SECURITY_DIR=str(root/'security'),BORG_UNKNOWN_UNENCRYPTED_REPO_ACCESS_IS_OK='yes')
            def borg(*args):
                return subprocess.run(['borg',*map(str,args)],env=env,check=True,capture_output=True,text=True,timeout=30).stdout
            borg('init','--encryption=none',repo)
            borg('config',repo,'storage_quota',64*1024*1024)
            self.assertEqual(int(borg('config',repo,'storage_quota').strip()),64*1024*1024)
            borg('create','--compression','zstd,9','--files-cache','disabled',str(repo)+'::full-scan',source)
            contents = borg('list','--format','{path}{NL}',str(repo)+'::full-scan').splitlines()
            self.assertTrue(any(path.endswith('example.txt') for path in contents))
            borg('config','--delete',repo,'storage_quota')

    def test_source_alias_cannot_silently_break_exclusions(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); (root/'actual').mkdir(); (root/'alias').symlink_to(root/'actual',target_is_directory=True)
            data = dict(self.data,source=str(root/'alias'))
            with patch.object(backend,'read_config',return_value=dict(data)), patch.object(backend,'atomic') as write:
                with self.assertRaisesRegex(ValueError,'resolved source path'): backend.apply(data)
                write.assert_not_called()

    def test_mount_is_idempotent_and_refuses_wrong_filesystem(self):
        with patch.object(backend,'mounted_at',return_value={'uuid':'expected'}), patch.object(backend,'run') as run:
            backend.ensure_mount('/backup','expected'); run.assert_not_called()
        with patch.object(backend,'mounted_at',return_value={'uuid':'wrong'}), patch.object(backend,'run') as run:
            with self.assertRaisesRegex(ValueError,'different filesystem'): backend.ensure_mount('/backup','expected')
            run.assert_not_called()

    def test_destination_creates_empty_repository_and_reuses_own_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); source=root/'source'; source.mkdir(); destination=root/'backup folder'
            data=dict(self.data,repository=str(destination))
            def run(*args,**kwargs):
                if args[0] == 'findmnt': return json.dumps({'filesystems':[{'target':str(root),'uuid':'abc-123'}]})
                env=dict(kwargs.get('env',os.environ),BORG_CACHE_DIR=str(root/'cache'),BORG_SECURITY_DIR=str(root/'security'))
                return subprocess.run(args,check=True,capture_output=True,text=True,env=env,timeout=30).stdout
            with patch.object(backend,'run',side_effect=run):
                backend.configure_destination(data,{},source)
                self.assertTrue((destination/'.m45-drive-backuper').is_file())
                # A valid repository from an earlier installation remains reusable.
                (destination/'.m45-drive-backuper').unlink()
                backend.configure_destination(data,{},source)
                self.assertTrue((destination/'config').is_file())
                self.assertFalse((destination/'.m45-drive-backuper').exists())
                self.assertEqual(data['mountpoint'],str(root))
                backend.configure_destination(data,{},source)
                text=backend.replacement(SCRIPT,data)
                self.assertIn('export BORG_REPO="$repository"',text)
                self.assertIn('--exclude "pp:${BORG_REPO#/}"',text)
                self.assertEqual(subprocess.run(['/bin/bash','-n'],input=text,text=True,capture_output=True).returncode,0)

    def test_destination_does_not_overwrite_files_or_contain_the_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); source=root/'source'; source.mkdir(); destination=root/'existing'; destination.mkdir(); (destination/'keep.txt').write_text('keep')
            output=json.dumps({'filesystems':[{'target':str(root),'uuid':'abc-123'}]})
            with patch.object(backend,'run',return_value=output):
                with self.assertRaisesRegex(ValueError,'contains other files'): backend.configure_destination(dict(self.data,repository=str(destination)),{},source)
                self.assertEqual((destination/'keep.txt').read_text(),'keep')
                with self.assertRaisesRegex(ValueError,'does not contain'): backend.configure_destination(dict(self.data,repository=str(root)),{},source)

    def test_mount_only_uses_matching_fstab_entry_and_rechecks_uuid(self):
        for valid in (True,False):
            entries={'filesystems':[{'target':'/backup/','uuid':'expected' if valid else 'wrong'}]}
            with patch.object(backend,'mounted_at',side_effect=[None,{'uuid':'expected'}]), patch.object(backend,'run',return_value=json.dumps(entries)) as run:
                if valid:
                    backend.ensure_mount('/backup','expected')
                    self.assertEqual(run.call_args_list[-1].args,('mount','--','/backup'))
                    self.assertEqual(run.call_args_list[-1].kwargs,{'timeout':45})
                else:
                    with self.assertRaisesRegex(ValueError,'fstab entry'): backend.ensure_mount('/backup','expected')
                    self.assertEqual(run.call_count,1)
        with patch.object(backend,'mounted_at',side_effect=[None,{'uuid':'wrong'}]), patch.object(backend,'run',return_value=json.dumps({'filesystems':[{'target':'/backup','uuid':'expected'}]})):
            with self.assertRaisesRegex(ValueError,'could not be mounted'): backend.ensure_mount('/backup','expected')

    def test_auto_mount_runs_before_any_source_checks_or_backup_cleanup(self):
        data=dict(self.data,auto_mount=True,source_uuid='source-id',source_mountpoint='/source-drive')
        text=backend.replacement(backend.replacement(SCRIPT,data),data)
        self.assertEqual(text.count('/usr/libexec/m45-drive-backuper/backend mount'),1)
        self.assertLess(text.index('/usr/libexec/m45-drive-backuper/backend mount'),text.index('[[ -d "$source" ]]'))
        self.assertLess(text.index('/usr/libexec/m45-drive-backuper/backend mount'),text.index('borg prune'))
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); script=root/'script'; excludes=root/'excludes'; interval=root/'interval'
            script.write_text(text); excludes.write_text(''); interval.write_text('OnCalendar=hourly\n')
            with patch.multiple(backend,SCRIPT=script,EXCLUDES=excludes,INTERVAL=interval):
                config=backend.read_config()
                self.assertTrue(config['auto_mount']); self.assertEqual(config['source_mountpoint'],'/source-drive')
                with patch.object(backend,'ensure_mount') as mount:
                    backend.prepare_mounts()
                    self.assertEqual(mount.call_args_list[0].args,('/source-drive','source-id'))
                    self.assertEqual(mount.call_args_list[1].args,(data['mountpoint'],config['uuid']))

    def test_borg_exclusions_keep_nearby_names_and_skip_selected_data(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); source=root/'source'; source.mkdir()
            (source/'Downloads').mkdir(); (source/'Downloads'/'skip.txt').write_text('skip')
            (source/'Downloads-keep').mkdir(); (source/'Downloads-keep'/'keep.txt').write_text('keep')
            (source/'a*?.txt').write_text('skip literal'); (source/'abc.txt').write_text('keep')
            (source/'project'/'node_modules').mkdir(parents=True); (source/'project'/'node_modules'/'dependency').write_text('skip')
            (source/'project'/'code.py').write_text('keep')
            rules=root/'exclude'; prefix=str(source).lstrip('/')
            rules.write_text(f'# Original patterns plus picker selections\nsh:{prefix}/**/node_modules\npp:{prefix}/Downloads\npf:{prefix}/a*?.txt\n')
            env=dict(os.environ,BORG_CACHE_DIR=str(root/'cache'),BORG_SECURITY_DIR=str(root/'security'),BORG_UNKNOWN_UNENCRYPTED_REPO_ACCESS_IS_OK='yes')
            def borg(*args):
                return subprocess.run(['borg',*map(str,args)],env=env,check=True,capture_output=True,text=True,timeout=30).stdout
            repo=root/'repo'; borg('init','--encryption=none',repo)
            borg('create','--exclude-from',rules,str(repo)+'::selection',source)
            paths=borg('list','--format','{path}{NL}',str(repo)+'::selection').splitlines()
            for suffix in ('Downloads/skip.txt','a*?.txt','node_modules/dependency'):
                self.assertFalse(any(path.endswith(suffix) for path in paths),suffix)
            for suffix in ('Downloads-keep/keep.txt','abc.txt','project/code.py'):
                self.assertTrue(any(path.endswith(suffix) for path in paths),suffix)

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
