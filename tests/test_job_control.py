import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1] / 'backend'))
import job_control as jobs
import idle_policy


class JobControlTests(unittest.TestCase):
    def setUp(self):
        tmp=tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        self.root=Path(tmp.name)
        p=patch.object(idle_policy,'SETTINGS',self.root/'idle-settings'); p.start(); self.addCleanup(p.stop)
        for name in ('STATE','LOCK','CGROUP'):
            p=patch.object(jobs,name,self.root/name); p.start(); self.addCleanup(p.stop)
        jobs.CGROUP.mkdir()
        self.props={kind:dict(ActiveState='inactive',InvocationID='') for kind in jobs.UNITS}
        self.config=dict(revision='a'*64)
        self.calls=[]
        def atomic(path,value,mode): path.write_text(value)
        def run(*args,**kwargs): self.calls.append(args); return ''
        self.request=self.root/'request'; self.request.write_text(json.dumps(dict(revision='a'*64,requested_at=0)))
        self.adapter=types.SimpleNamespace(read_config=lambda:dict(self.config),
            properties=lambda unit:dict(self.props[next(k for k,v in jobs.UNITS.items() if v==unit)]),
            atomic=atomic,run=run,verification=types.SimpleNamespace(REQUEST=self.request))

    def running(self,kind='backup'):
        self.props[kind]=dict(ActiveState='activating',InvocationID='invocation')
        jobs.freezer(kind).parent.mkdir()
        jobs.freezer(kind).write_text('0')
        jobs.freezer(kind).with_name('cgroup.events').write_text('populated 1\nfrozen 1\n')

    def request_data(self,op,kind='backup',delay=0):
        return dict(kind=kind,operation=op,delay=delay,revision=self.config['revision'],
                    invocation=self.props[kind]['InvocationID'] if self.props[kind]['ActiveState'] in jobs.ACTIVE else '')

    def test_invalid_or_stale_requests_cannot_touch_jobs(self):
        for update in (dict(kind='../../other'),dict(operation='kill'),dict(delay=True),dict(delay=3601),dict(revision='stale'),dict(invocation='stale')):
            with self.subTest(update=update), self.assertRaises(ValueError):
                jobs.control(self.adapter,dict(self.request_data('pause'),**update))
        self.assertEqual(self.calls,[])

    def test_pause_freezes_whole_oneshot_and_resume_does_not_restart(self):
        self.running()
        jobs.control(self.adapter,self.request_data('pause'))
        self.assertEqual(jobs.freezer('backup').read_text(),'1')
        self.assertFalse(jobs.allowed('backup'))
        self.assertTrue(jobs.status()['backup']['paused'])
        jobs.control(self.adapter,self.request_data('resume'))
        self.assertEqual(jobs.freezer('backup').read_text(),'0')
        self.assertTrue(jobs.allowed('backup'))
        self.assertFalse(any(c[1]=='start' for c in self.calls))

    def test_active_postponement_resumes_once_without_starting_again(self):
        self.running('check')
        jobs.control(self.adapter,self.request_data('postpone','check',3600))
        until=jobs.read()['check']['until']
        jobs.tick(self.adapter,until-1)
        self.assertEqual(jobs.freezer('check').read_text(),'1')
        jobs.tick(self.adapter,until+1); jobs.tick(self.adapter,until+2)
        self.assertEqual(jobs.freezer('check').read_text(),'0')
        self.assertTrue(jobs.allowed('check'))
        self.assertFalse(any(c[1]=='start' for c in self.calls))

    def test_inactive_postponement_launches_one_run_without_changing_schedule(self):
        jobs.control(self.adapter,self.request_data('postpone',delay=3600))
        until=jobs.read()['backup']['until']
        jobs.tick(self.adapter,until+1); jobs.tick(self.adapter,until+2)
        self.assertEqual(self.calls.count(('systemctl','start','--no-block',jobs.UNITS['backup'])),1)
        self.assertFalse(any(c[1] in ('disable','restart') for c in self.calls))

    def test_reboot_or_replacement_never_resumes_an_unrelated_job(self):
        self.running()
        jobs.control(self.adapter,self.request_data('postpone',delay=3600))
        until=jobs.read()['backup']['until']
        self.props['backup']['InvocationID']='replacement'
        jobs.tick(self.adapter,until+1)
        self.assertEqual(jobs.freezer('backup').read_text(),'1')
        self.assertTrue(jobs.allowed('backup'))
        self.assertFalse(any(c[1]=='start' for c in self.calls))

    def test_changed_plan_cancels_pending_run(self):
        jobs.control(self.adapter,self.request_data('postpone',delay=3600))
        self.config['revision']='b'*64
        jobs.tick(self.adapter,jobs.read()['backup']['until']+1)
        self.assertTrue(jobs.allowed('backup'))
        self.assertFalse(any(c[1]=='start' for c in self.calls))

    def test_cancel_thaws_and_clears_queued_selection_and_gate(self):
        self.running('check')
        jobs.control(self.adapter,self.request_data('pause','check'))
        jobs.control(self.adapter,self.request_data('cancel','check'))
        self.assertEqual(jobs.freezer('check').read_text(),'0')
        self.assertFalse(self.request.exists())
        self.assertTrue(jobs.allowed('check'))
        self.assertIn(('systemctl','stop','--no-block',jobs.UNITS['check']),self.calls)

    def test_start_failure_keeps_pending_request_for_retry(self):
        jobs.control(self.adapter,self.request_data('postpone',delay=3600))
        self.adapter.run=lambda *a,**kw: (_ for _ in ()).throw(RuntimeError('temporary failure'))
        with self.assertRaises(RuntimeError): jobs.tick(self.adapter,jobs.read()['backup']['until']+1)
        self.assertFalse(jobs.allowed('backup'))

    def test_failed_freeze_does_not_leave_new_gate(self):
        self.props['backup']=dict(ActiveState='activating',InvocationID='invocation')
        with self.assertRaises(OSError): jobs.control(self.adapter,self.request_data('pause'))
        self.assertTrue(jobs.allowed('backup'))

    def test_pause_future_runs_resume_does_not_start_a_backup(self):
        jobs.control(self.adapter,self.request_data('pause'))
        self.assertFalse(jobs.allowed('backup'))
        jobs.control(self.adapter,self.request_data('resume'))
        self.assertTrue(jobs.allowed('backup'))
        self.assertFalse(any(c[1]=='start' for c in self.calls))

    def test_paused_selected_check_keeps_selection_fresh(self):
        self.running('check')
        pending=dict(revision='a'*64,requested_at=0,selection=dict(scope='archive',archive_id='b'*64))
        self.request.write_text(json.dumps(pending))
        jobs.control(self.adapter,self.request_data('pause','check'))
        jobs.control(self.adapter,self.request_data('resume','check'))
        restored=json.loads(self.request.read_text())
        self.assertEqual(restored['selection'],pending['selection'])
        self.assertGreater(restored['requested_at'],0)
