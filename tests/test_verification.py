import contextlib
import fcntl
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'backend'))
import verification
import idle_policy
import job_control


class VerificationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        for module,name in ((idle_policy,'SETTINGS'),(job_control,'STATE'),(job_control,'LOCK')):
            patcher=patch.object(module,name,self.root/('idle-'+name)); patcher.start(); self.addCleanup(patcher.stop)
        for name in ('SETTINGS', 'STATE', 'TIMER_DROPIN', 'REQUEST'):
            patcher = patch.object(verification, name, self.root / name)
            patcher.start(); self.addCleanup(patcher.stop)
        self.config = dict(repository=str(self.root/'repository'), uuid='test-uuid', mountpoint=str(self.root), source=str(self.root/'source'), revision='a'*64, auto_mount=False)
        self.calls = []
        def atomic(path, value, mode):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(value); path.chmod(mode)
        def run(*args, **kwargs):
            self.calls.append(args)
            return 'test-uuid' if args[0] == 'findmnt' else ''
        script = self.root/'script'
        script.write_text('#!/bin/bash\nexec 9>/tmp/lock\nflock -n 9 || exit 0\nbackup_rc=0\nm45_finished=1\nexit "$backup_rc"\n')
        self.adapter = types.SimpleNamespace(read_config=lambda: dict(self.config), properties=lambda unit: dict(ActiveState='inactive', UnitFileState='disabled'),
                                             LOCK=self.root/'lock', SCRIPT=script, BACKUPS=self.root/'backups', atomic=atomic, run=run, ENV=dict(os.environ), prepare_mounts=lambda: None)

    def test_manual_is_default_and_reading_does_not_enable_anything(self):
        result = verification.status(self.config, self.adapter.properties)
        self.assertEqual(result['mode'], 'manual'); self.assertEqual(result['scope'],'latest'); self.assertEqual(result['attempt'], {})
        self.assertFalse(verification.SETTINGS.exists()); self.assertFalse(self.calls)
        verification.request(self.adapter, after_backup=True)
        self.assertFalse(self.calls)

    def test_hook_runs_after_warnings_and_never_turns_failed_backup_into_success(self):
        script = verification.hook_script(self.adapter.SCRIPT.read_text())
        self.assertEqual(verification.hook_script(script), script)
        self.assertIn('flock 9\n', script)
        self.assertIn('if (( backup_rc <= 1 )); then', script)
        self.assertLess(script.index('verification-after-backup'), script.index('m45_finished=1'))
        self.assertTrue(script.endswith('exit "$backup_rc"\n'))
        self.assertEqual(subprocess.run(['bash','-n'],input=script,text=True).returncode,0)
        with self.assertRaises(ValueError): verification.hook_script('echo unsupported\n')

    def test_completion_hook_executes_for_warning_but_not_fatal_backup(self):
        event=self.root/'event'; helper=self.root/'helper'
        helper.write_text('#!/bin/bash\necho queued >> '+str(event)+'\nexit 9\n'); helper.chmod(0o700)
        for rc in (0,1,2):
            event.unlink(missing_ok=True)
            # Apply to a supported shape, then vary the runtime backup result.
            script=verification.hook_script(self.adapter.SCRIPT.read_text()).replace('/tmp/lock',str(self.root/'job-lock')).replace('backup_rc=0','backup_rc='+str(rc))
            script=script.replace('/usr/libexec/m45-drive-backuper/backend',str(helper))
            completed=subprocess.run(['bash'],input=script,text=True,capture_output=True)
            self.assertEqual(completed.returncode,rc)
            self.assertEqual(event.exists(),rc<=1)

    def test_schedules_are_explicit_and_manual_disables_timer(self):
        for mode in verification.MODES:
            verification.configure(dict(mode=mode,scope='latest',revision=verification.revision(self.config)),self.adapter)
            self.assertEqual(verification.settings()['mode'], mode)
            units=self.root/'units'; units.mkdir(exist_ok=True)
            packaging=Path(__file__).resolve().parents[1]/'packaging'
            for name in ('m45-backup-check.service','m45-backup-check.timer'):
                content=(packaging/name).read_text()
                # Validate unit semantics without requiring this app to be
                # installed on the test/CI host. No service is started.
                content=content.replace('/usr/libexec/m45-drive-backuper/backend verification-run','/usr/bin/true')
                (units/name).write_text(content)
            dropin=units/'m45-backup-check.timer.d'; dropin.mkdir(exist_ok=True)
            (dropin/'schedule.conf').write_text(verification.TIMER_DROPIN.read_text())
            checked=subprocess.run(['systemd-analyze','verify','--man=no',str(units/'m45-backup-check.service'),str(units/'m45-backup-check.timer')],capture_output=True,text=True)
            self.assertEqual(checked.returncode,0,checked.stderr)
            enabled=('systemctl','enable','--now',verification.TIMER) in self.calls
            self.assertEqual(enabled, bool(verification.MODES[mode]))
            self.calls.clear()
        with self.assertRaises(ValueError): verification.configure(dict(mode='weekly',scope='latest',revision='stale'),self.adapter)

    def test_failed_save_rolls_back_settings_and_script(self):
        original=self.adapter.SCRIPT.read_text()
        def fail(*args, **kwargs):
            if args[:3] == ('systemctl','enable','--now') and kwargs.get('check', True): raise RuntimeError('systemd unavailable')
            return ''
        self.adapter.run=fail
        with self.assertRaises(RuntimeError): verification.configure(dict(mode='daily',scope='latest',revision=verification.revision(self.config)),self.adapter)
        self.assertEqual(self.adapter.SCRIPT.read_text(),original)
        self.assertFalse(verification.SETTINGS.exists())
        self.assertFalse(verification.TIMER_DROPIN.exists())

    def test_lock_contention_is_deferred_without_running_borg(self):
        with self.adapter.LOCK.open('a') as lock, patch.object(verification.subprocess,'Popen') as popen:
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
            result=verification.run_check(self.adapter)
            self.assertEqual(result['_exit_code'],75); popen.assert_not_called()
        self.assertEqual(json.loads(verification.STATE.read_text())['attempt']['outcome'],'deferred')

    def test_wrong_mount_is_error_and_previous_success_is_not_overwritten(self):
        previous=dict(finished_at='2026-11-15T00:00:00+00:00')
        verification.STATE.write_text(json.dumps(dict(identity=verification.identity(self.config),last_success=previous)))
        self.adapter.run=lambda *args,**kwargs:'wrong-uuid'
        with patch.object(verification.subprocess,'Popen') as popen:
            self.assertEqual(verification.run_check(self.adapter)['_exit_code'],2); popen.assert_not_called()
        state=json.loads(verification.STATE.read_text())
        self.assertEqual(state['attempt']['outcome'],'error'); self.assertIn('expected backup filesystem',state['attempt']['message']); self.assertEqual(state['last_success'],previous)
        self.config['repository']='different'
        self.assertEqual(verification.status(self.config,self.adapter.properties)['last_success'],{})

    def test_failed_service_cannot_display_old_success_as_current_success(self):
        previous=dict(outcome='success',finished_at='yesterday')
        verification.STATE.write_text(json.dumps(dict(identity=verification.identity(self.config),attempt=previous,last_success=previous)))
        result=verification.status(self.config,lambda unit:dict(ActiveState='failed' if unit==verification.SERVICE else 'inactive'))
        self.assertEqual(result['attempt']['outcome'],'error')
        self.assertEqual(result['last_success'],previous)

    def test_interrupted_check_retains_previous_success(self):
        verification.SETTINGS.write_text(json.dumps(dict(mode='manual',scope='all')))
        previous=dict(outcome='success',finished_at='yesterday')
        verification.STATE.write_text(json.dumps(dict(identity=verification.identity(self.config),attempt=previous,last_success=previous)))
        class InterruptedOutput:
            def __iter__(self): raise InterruptedError('Verification stopped before it finished.')
            def close(self): pass
        class Process:
            stdout=InterruptedOutput()
            terminated=False
            def poll(self): return 130 if self.terminated else None
            def terminate(self): self.terminated=True
            def wait(self,timeout=None): return 130
        with patch.object(verification.subprocess,'Popen',return_value=Process()):
            self.assertEqual(verification.run_check(self.adapter)['_exit_code'],130)
        state=json.loads(verification.STATE.read_text())
        self.assertEqual(state['attempt']['outcome'],'interrupted')
        self.assertEqual(state['last_success'],previous)

    def test_queue_failure_is_persisted_even_after_backup(self):
        verification.SETTINGS.write_text(json.dumps(dict(mode='after_backup')))
        self.adapter.run=lambda *args,**kwargs: (_ for _ in ()).throw(RuntimeError('service unavailable'))
        with self.assertRaises(RuntimeError): verification.request(self.adapter,after_backup=True)
        self.assertEqual(json.loads(verification.STATE.read_text())['attempt']['outcome'],'error')

    def test_latest_and_selected_scope_skip_checkpoints_and_validate_ids(self):
        catalog=[dict(name='home-old',id='a'*64,start='2026-11-15'),dict(name='home-new',id='b'*64,start='2026-11-16'),
                 dict(name='home-new.checkpoint.1',id='c'*64,start='2026-11-17')]
        self.adapter.run=lambda *args,**kwargs:json.dumps(dict(archives=catalog))
        command,target=verification.check_command(self.adapter,self.config,dict(scope='latest'),{})
        self.assertEqual(target['archive_id'],'b'*64)
        self.assertEqual(command[1:3],['extract','--dry-run'])
        self.assertTrue(command[-1].endswith('::home-new'))
        command,target=verification.check_command(self.adapter,self.config,dict(scope='archive',archive_id='a'*64),{})
        self.assertEqual(target['archive'],'home-old')
        with self.assertRaises(ValueError): verification.check_command(self.adapter,self.config,dict(scope='archive',archive_id='c'*64),{})
        with self.assertRaises(ValueError): verification.check_command(self.adapter,self.config,dict(scope='archive',archive_id='d'*64),{})
        self.adapter.run=lambda *args,**kwargs:json.dumps(dict(archives=[]))
        with self.assertRaises(ValueError): verification.check_command(self.adapter,self.config,dict(scope='latest'),{})

    def test_manual_scope_request_does_not_change_schedule_and_stale_requests_fail(self):
        saved=dict(mode='weekly',scope='latest')
        verification.SETTINGS.write_text(json.dumps(saved))
        verification.request(self.adapter,selection=dict(scope='all',archive_id='',revision=self.config['revision']))
        self.assertEqual(verification.settings(),saved)
        request=json.loads(verification.REQUEST.read_text())
        self.assertEqual(request['selection']['scope'],'all')
        request['requested_at']=0; verification.REQUEST.write_text(json.dumps(request))
        with patch.object(verification.subprocess,'Popen') as popen:
            self.assertEqual(verification.run_check(self.adapter)['_exit_code'],2)
            popen.assert_not_called()
        self.assertFalse(verification.REQUEST.exists())
        self.assertIn('expired',json.loads(verification.STATE.read_text())['attempt']['message'])
        for selection in (dict(scope='archive',archive_id='--repair',revision=self.config['revision']),
                          dict(scope='latest',archive_id='',revision='stale')):
            with self.assertRaises(ValueError): verification.request(self.adapter,selection=selection)

    def test_latest_read_ignores_old_only_corruption_but_selected_old_check_detects_it(self):
        source=self.root/'source'; source.mkdir(); (source/'old-only').write_bytes(os.urandom(200000))
        repo=self.root/'repository'; sandbox=self.root/'no-restored-files'; sandbox.mkdir()
        env=dict(os.environ,BORG_CACHE_DIR=str(self.root/'cache'),BORG_SECURITY_DIR=str(self.root/'security'),BORG_UNKNOWN_UNENCRYPTED_REPO_ACCESS_IS_OK='yes')
        def borg(*args): return subprocess.run(['borg',*args],env=env,text=True,check=True,capture_output=True).stdout
        borg('init','--encryption=none',str(repo)); borg('create',str(repo)+'::home-old',str(source))
        old_segment=max((repo/'data').glob('*/*'),key=lambda p:p.stat().st_size)
        (source/'old-only').unlink(); (source/'latest-only').write_bytes(os.urandom(2000))
        borg('create',str(repo)+'::home-new',str(source))
        old_id=next(a['id'] for a in json.loads(borg('list','--json',str(repo)))['archives'] if a['name']=='home-old')
        with old_segment.open('r+b') as stream:
            stream.seek(70); value=stream.read(1); stream.seek(70); stream.write(bytes([value[0]^1]))
        def adapter_run(*args,**kwargs):
            if args[0]=='findmnt': return 'test-uuid'
            if args[0]=='borg': return subprocess.run(args,env=env,text=True,check=True,capture_output=True).stdout
            return ''
        self.adapter.run=adapter_run
        prior_full=dict(scope='all',outcome='success',finished_at='yesterday')
        verification.STATE.write_text(json.dumps(dict(identity=verification.identity(self.config),last_success=prior_full)))
        real_popen=subprocess.Popen
        def isolated(*args,**kwargs):
            kwargs.update(env=env,cwd=str(sandbox))
            return real_popen(*args,**kwargs)
        with patch.object(verification.subprocess,'Popen',side_effect=isolated),contextlib.redirect_stdout(io.StringIO()):
            latest=verification.run_check(self.adapter)
        self.assertEqual(latest['_exit_code'],0,json.loads(verification.STATE.read_text()))
        state=json.loads(verification.STATE.read_text())
        self.assertEqual(state['last_success']['archive'],'home-new')
        self.assertEqual(state['last_full_success'],prior_full)
        self.assertEqual(list(sandbox.iterdir()),[])
        verification.request(self.adapter,selection=dict(scope='archive',archive_id=old_id,revision=self.config['revision']))
        with patch.object(verification.subprocess,'Popen',side_effect=isolated),contextlib.redirect_stdout(io.StringIO()):
            older=verification.run_check(self.adapter)
        self.assertNotEqual(older['_exit_code'],0)
        state=json.loads(verification.STATE.read_text())
        self.assertEqual(state['attempt']['archive'],'home-old')
        self.assertEqual(state['last_success']['archive'],'home-new')
        self.assertEqual(state['last_full_success'],prior_full)
        self.assertEqual(list(sandbox.iterdir()),[])

    def test_real_borg_full_check_and_missing_data_are_detected(self):
        verification.SETTINGS.write_text(json.dumps(dict(mode='manual',scope='all')))
        source=self.root/'source'; source.mkdir(); (source/'precious.txt').write_bytes(os.urandom(1024*100))
        repo=self.root/'repository'
        env=dict(os.environ,BORG_CACHE_DIR=str(self.root/'cache'),BORG_SECURITY_DIR=str(self.root/'security'),BORG_UNKNOWN_UNENCRYPTED_REPO_ACCESS_IS_OK='yes')
        for args in (['init','--encryption=none',str(repo)],['create',str(repo)+'::home-test',str(source)]):
            subprocess.run(['borg',*args],env=env,check=True,capture_output=True)
        # The production runner supplies fixed root cache paths; isolate only
        # those environment paths while exercising the actual Borg subprocess.
        real_popen=subprocess.Popen
        def isolated(*args,**kwargs):
            kwargs['env']=env
            return real_popen(*args,**kwargs)
        with patch.object(verification.subprocess,'Popen',side_effect=isolated),contextlib.redirect_stdout(io.StringIO()):
            result=verification.run_check(self.adapter)
        self.assertEqual(result['_exit_code'],0,json.loads(verification.STATE.read_text()))
        successful=json.loads(verification.STATE.read_text())['last_success']
        self.assertEqual(successful['outcome'],'success')
        # Corrupt bytes in a real segment; metadata listing alone is insufficient.
        segment=max((repo/'data').glob('*/*'),key=lambda p:p.stat().st_size)
        with segment.open('r+b') as stream:
            stream.seek(60); value=stream.read(1); stream.seek(60); stream.write(bytes([value[0]^1]))
        with patch.object(verification.subprocess,'Popen',side_effect=isolated),contextlib.redirect_stdout(io.StringIO()):
            result=verification.run_check(self.adapter)
        self.assertNotEqual(result['_exit_code'],0)
        state=json.loads(verification.STATE.read_text())
        self.assertIn(state['attempt']['outcome'],('warning','error'))
        self.assertEqual(state['last_success'],successful)


if __name__=='__main__': unittest.main()
