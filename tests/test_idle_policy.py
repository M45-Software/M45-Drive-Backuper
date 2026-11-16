import json
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'backend'))
import idle_policy as policy
import job_control as jobs


class IdlePolicyTests(unittest.TestCase):
    def setUp(self):
        tmp=tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup); self.root=Path(tmp.name)
        for module,name in ((policy,'SETTINGS'),(jobs,'STATE'),(jobs,'LOCK')):
            p=patch.object(module,name,self.root/name); p.start(); self.addCleanup(p.stop)
        self.config=dict(revision='a'*64)
        self.calls=[]
        self.idle=False; self.available=True; self.check_revision='check-revision'
        def atomic(path,value,mode): path.write_text(value)
        self.adapter=types.SimpleNamespace(read_config=lambda:dict(self.config),
            properties=lambda unit:dict(ActiveState='inactive'),atomic=atomic,
            run=lambda *a,**kw:self.calls.append(a) or '',
            verification=types.SimpleNamespace(revision=lambda c:self.check_revision))
        self.probe=patch.object(policy,'desktop_idle',side_effect=lambda *a,**kw:(self.idle,'test idle state',self.available))

    def enable(self,hours=6):
        policy.SETTINGS.write_text(json.dumps(dict(backup=True,check=True,max_hours=hours)))

    def test_defaults_are_off_with_six_hour_maximum(self):
        self.assertEqual(policy.settings(),dict(backup=False,check=False,max_hours=6))
        with self.probe:
            self.assertTrue(jobs.gate(self.adapter,'backup',1000))
        self.assertFalse(jobs.STATE.exists())

    def test_repeated_triggers_do_not_extend_deadline_and_busy_jobs_run_at_limit(self):
        self.enable()
        with self.probe:
            self.assertFalse(jobs.gate(self.adapter,'backup',1000))
            self.assertEqual(jobs.read()['backup']['until'],22600)
            self.assertFalse(jobs.gate(self.adapter,'backup',2000))
            self.assertEqual(jobs.read()['backup']['until'],22600)
            jobs.tick(self.adapter,22599); self.assertEqual(self.calls,[])
            jobs.tick(self.adapter,22600)
            self.assertIn(('systemctl','start','--no-block',jobs.UNITS['backup']),self.calls)
            # A released job gets exactly one pass through ExecCondition.
            self.assertTrue(jobs.gate(self.adapter,'backup'))
            self.assertFalse(jobs.gate(self.adapter,'backup'))

    def test_idle_arrival_runs_early_without_holding_repository_lock(self):
        self.enable()
        with self.probe:
            jobs.gate(self.adapter,'check',1000)
            self.assertEqual(jobs.read()['check']['invocation'],'')
            self.idle=True; jobs.tick(self.adapter,1001)
            self.assertNotIn('check',jobs.read())
            jobs.tick(self.adapter,1002)
        self.assertEqual(self.calls.count(('systemctl','start','--no-block',jobs.UNITS['check'])),1)

    def test_unavailable_reporting_disables_deferral_and_releases_waiters(self):
        self.enable()
        with self.probe:
            jobs.gate(self.adapter,'backup',1000)
            self.available=False
            jobs.tick(self.adapter,1001)
            self.assertNotIn('backup',jobs.read())
            self.assertTrue(jobs.gate(self.adapter,'check',1001))
            self.assertNotIn('check',jobs.read())
            status=policy.status(self.adapter.run)
            self.assertFalse(status['available'])
            self.assertIn('disabled',status['message'])

    def test_manual_start_bypasses_idle_wait_but_not_explicit_pause(self):
        self.enable()
        with self.probe:
            jobs.gate(self.adapter,'backup')
            self.assertTrue(jobs.allowed('backup',manual=True))
            jobs.manual_start(self.adapter,'backup')
            self.assertTrue(jobs.gate(self.adapter,'backup'))
            self.assertFalse(jobs.gate(self.adapter,'backup'))
            jobs.save(self.adapter,dict(backup=dict(mode='paused')))
            self.assertFalse(jobs.allowed('backup',manual=True))
            with self.assertRaises(ValueError): jobs.manual_start(self.adapter,'backup')

    def test_changed_plan_or_check_options_cannot_start_stale_waiter(self):
        self.enable()
        with self.probe:
            jobs.gate(self.adapter,'backup',1000); jobs.gate(self.adapter,'check',1000)
            self.config['revision']='b'*64
            jobs.tick(self.adapter,999999)
            self.assertNotIn('backup',jobs.read()); self.assertNotIn('check',jobs.read())
            jobs.gate(self.adapter,'check',1000)
            self.check_revision='new-check-options'
            jobs.tick(self.adapter,999999)
        self.assertEqual(self.calls,[])

    def test_disabling_option_releases_pending_run_and_cap_changes_never_extend_it(self):
        self.enable(48)
        with self.probe:
            jobs.gate(self.adapter,'backup',1000)
            def configure(backup,check,hours):
                policy.configure(self.adapter,dict(backup=backup,check=check,max_hours=hours,revision=policy.revision(policy.settings())))
            configure(True,True,6)
            self.assertEqual(jobs.read()['backup']['until'],22600)
            configure(True,True,48)
            self.assertEqual(jobs.read()['backup']['until'],22600)
            configure(False,False,6)
            self.assertNotIn('backup',jobs.read())
        self.assertIn(('systemctl','start','--no-block',jobs.UNITS['backup']),self.calls)

    def test_invalid_stale_or_unsupported_options_cannot_enable_feature(self):
        data=dict(backup=True,check=False,max_hours=6,revision=policy.revision(policy.settings()))
        with self.probe:
            for change in (dict(max_hours=True),dict(max_hours=8),dict(check=1),dict(revision='old')):
                with self.subTest(change=change),self.assertRaises(ValueError): policy.configure(self.adapter,dict(data,**change))
            self.available=False
            with self.assertRaises(ValueError): policy.configure(self.adapter,data)
        self.assertEqual(self.calls,[])
        self.assertFalse(policy.SETTINGS.exists())

    def test_real_probe_requires_all_sessions_and_valid_idle_hints(self):
        def probe(props):
            adapter=types.SimpleNamespace(run=lambda *a,**kw:'4 1000 user\n5 1001 other' if a[1]=='list-sessions' else props[a[2]])
            return policy.desktop_idle(adapter,monotonic=1000)
        idle='Type=wayland\nClass=user\nIdleHint=yes\nIdleSinceHintMonotonic=100000000'
        busy='Type=x11\nClass=user\nIdleHint=no\nIdleSinceHintMonotonic=0'
        self.assertEqual(probe({'4':idle,'5':idle})[::2],(True,True))
        self.assertEqual(probe({'4':idle,'5':busy})[::2],(False,True))
        self.assertEqual(probe({'4':idle,'5':'Type=unknown\nClass=user'})[::2],(False,False))
        self.assertEqual(probe({'4':idle,'5':idle.replace('100000000','900000000')})[::2],(False,True))
        self.assertEqual(probe({'4':idle,'5':idle.replace('100000000','0')})[::2],(False,False))
        self.assertTrue(policy.desktop_idle(types.SimpleNamespace(run=lambda *a,**kw:''),1000)[0])
        for error in (RuntimeError('unavailable'),subprocess.TimeoutExpired('loginctl',2)):
            def fail(*a,**kw): raise error
            self.assertEqual(policy.desktop_idle(types.SimpleNamespace(run=fail),1000)[::2],(False,False))
