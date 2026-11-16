import base64
import contextlib
import io
import json
import os
from pathlib import Path
import pwd
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import types
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'backend'))
import file_recovery


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory(); self.addCleanup(self.temporary.cleanup)
        self.root=Path(self.temporary.name)
        for name,value in [('CACHE',self.root/'catalogues'),('BORG_CACHE',self.root/'borg-cache'),('BORG_SECURITY',self.root/'borg-security'),('ACTIVITY',self.root/'activity.json')]:
            patched=patch.object(file_recovery,name,value); patched.start(); self.addCleanup(patched.stop)
        self.env=dict(os.environ,BORG_CACHE_DIR=str(self.root/'borg-cache'),BORG_SECURITY_DIR=str(self.root/'borg-security'),BORG_UNKNOWN_UNENCRYPTED_REPO_ACCESS_IS_OK='yes')
        self.source=self.root/'source'; self.source.mkdir()
        docs=self.source/'Documents'; docs.mkdir(); (docs/'nested').mkdir()
        (docs/'one.txt').write_text('backed up version')
        (docs/'nested'/'two.txt').write_text('nested content')
        (docs/'a[1]*.txt').write_text('literal name'); (docs/'a11-other.txt').write_text('not selected')
        os.link(docs/'one.txt',docs/'hardlink.txt')
        os.symlink('../one.txt',docs/'nested'/'link.txt')
        raw=os.fsencode(docs)+b'/bad-\xff.txt'
        with open(raw,'wb') as stream: stream.write(b'raw filename')
        self.repo=self.root/'repository'
        self.borg('init','--encryption=none',str(self.repo)); self.borg('create',str(self.repo)+'::home-test',str(self.source))
        self.archive_id=json.loads(self.borg('list','--json',str(self.repo)))['archives'][0]['id']
        self.config=dict(repository=str(self.repo),mountpoint=str(self.root),source=str(self.source),uuid='test-uuid',revision='a'*64,auto_mount=False)
        def atomic(path,content,mode):
            path.parent.mkdir(parents=True,exist_ok=True); path.write_text(content); path.chmod(mode)
        self.adapter=types.SimpleNamespace(atomic=atomic,__file__=str(Path(__file__).resolve().parents[1]/'backend/m45-drive-backuper.py'),
            LOCK=self.root/'job-lock',ENV=self.env,read_config=lambda:dict(self.config),run=lambda *args,**kwargs:'test-uuid')
        with file_recovery.saved_archive(self.config,self.archive_id) as archive:
            self.db=file_recovery.catalogue(self.config,self.archive_id,archive,lambda message:None)
        self.addCleanup(self.db.close)
        self.parent=os.fsencode(str(docs).lstrip('/'))

    def borg(self,*args):
        return subprocess.run(['borg',*args],env=self.env,text=True,capture_output=True,check=True).stdout

    def test_recovery_activity_cannot_stay_running_after_completion_or_pid_reuse(self):
        with file_recovery.activity(self.adapter,self.config,'recover'):
            self.assertTrue(file_recovery.status(self.config)['active'])
            self.assertFalse(file_recovery.status(dict(self.config,repository='another'))['active'])
        self.assertFalse(file_recovery.status(self.config)['active'])
        value=json.loads(file_recovery.ACTIVITY.read_text()); value.update(active=True,start_ticks='stale')
        file_recovery.ACTIVITY.write_text(json.dumps(value))
        self.assertFalse(file_recovery.status(self.config)['active'])

    def test_catalogue_browses_real_metadata_and_raw_names(self):
        result=file_recovery.browse(self.db,self.parent)
        names={entry['name']:entry for entry in result['entries']}
        self.assertIn('nested',names); self.assertIn('bad-\\xff.txt',names)
        self.assertEqual(file_recovery.raw_path(names['bad-\\xff.txt']['token']),self.parent+b'/bad-\xff.txt')
        self.assertEqual(names['hardlink.txt']['size'],len('backed up version'))
        self.assertEqual(names['nested']['kind'],'d')
        with file_recovery.saved_archive(self.config,'latest') as archive:
            self.assertEqual(archive.id.hex(),self.archive_id)
        self.assertEqual(file_recovery.browse(self.db,self.parent,search='[1]*')['total'],1)
        with patch.object(file_recovery,'saved_archive',side_effect=AssertionError('must not re-read backup during navigation')):
            self.assertEqual(file_recovery.browse(self.db,self.parent,search='one.txt')['total'],1)

    def test_locked_repository_fails_without_prompting_on_browser_stdin(self):
        encrypted=self.root/'encrypted'
        env=dict(self.env,BORG_PASSPHRASE='test-secret')
        subprocess.run(['borg','init','--encryption=repokey',str(encrypted)],env=env,check=True,capture_output=True)
        subprocess.run(['borg','create',str(encrypted)+'::home-locked',str(self.source)],env=env,check=True,capture_output=True)
        config=dict(self.config,repository=str(encrypted))
        with patch.dict(os.environ,{},clear=True),patch.object(sys,'stdin',io.StringIO('browser input must not be consumed')) as stdin:
            with self.assertRaises(Exception):
                with file_recovery.saved_archive(config,'latest'): pass
            self.assertEqual(stdin.tell(),0)

    def test_traversal_and_cross_folder_selection_are_rejected(self):
        for path in (b'../escape',b'/etc/passwd',b'a/../b',b'a//b',b'a\0b'):
            with self.assertRaises(ValueError): file_recovery.raw_path(file_recovery.token(path))
        with self.assertRaises(ValueError): file_recovery.selected_paths(self.db,self.parent,[file_recovery.token(self.parent+b'/nested/two.txt')])
        with self.assertRaises(ValueError): file_recovery.selected_paths(self.db,self.parent,[])
        with self.assertRaises(ValueError):
            with file_recovery.saved_archive(self.config,'f'*64): pass

    def run_recovery(self,names,base):
        owner=pwd.getpwuid(os.getuid()); real_popen=subprocess.Popen; identities=[]
        def spawn(*args,**kwargs):
            if 'user' in kwargs:
                identities.append((kwargs['user'],kwargs['group']))
                # The test already runs as the desktop user. Production starts
                # this exact worker with these credentials from the root broker.
                for key in ('user','group','extra_groups'): kwargs.pop(key)
            return real_popen(*args,**kwargs)
        request=dict(command='recover',path=file_recovery.token(self.parent),
                     items=[file_recovery.token(self.parent+b'/'+name) for name in names],destination=str(base))
        with patch.object(file_recovery.subprocess,'Popen',side_effect=spawn):
            result=file_recovery.recover(self.adapter,self.config,self.archive_id,self.db,request,owner,notify=lambda *a,**k:None)
        self.assertEqual(identities,[(owner.pw_uid,owner.pw_gid)])
        return result

    @unittest.skipIf(os.geteuid()==0,'The recovery worker deliberately refuses root; this pipeline test runs as a desktop user.')
    def test_single_file_and_folder_recovery_never_overwrite_live_files(self):
        base=self.root/'Recovered Files'; base.mkdir(); (base/'one.txt').write_text('existing output')
        (self.source/'Documents'/'one.txt').write_text('live changed version')
        result=self.run_recovery([b'one.txt',b'a[1]*.txt',b'bad-\xff.txt'],base)
        self.assertTrue(result['ok'],result); output=Path(result['path'])
        self.assertEqual((output/'one.txt').read_text(),'backed up version')
        self.assertEqual((output/'a[1]*.txt').read_text(),'literal name')
        self.assertFalse((output/'a11-other.txt').exists())
        self.assertEqual((base/'one.txt').read_text(),'existing output')
        self.assertEqual((self.source/'Documents'/'one.txt').read_text(),'live changed version')
        self.assertEqual((output/'one.txt').stat().st_uid,os.getuid())
        self.assertEqual((output/'bad-\udcff.txt').read_bytes(),b'raw filename')
        folder=self.run_recovery([b'nested'],base)
        self.assertTrue(folder['ok'],folder); self.assertNotEqual(folder['path'],result['path'])
        self.assertEqual((Path(folder['path'])/'nested'/'two.txt').read_text(),'nested content')
        hardlink=self.run_recovery([b'hardlink.txt'],base)
        self.assertTrue(hardlink['ok'],hardlink)
        self.assertEqual((Path(hardlink['path'])/'hardlink.txt').read_text(),'backed up version')

    @unittest.skipIf(os.geteuid()==0,'The recovery worker deliberately refuses root.')
    def test_export_failure_never_claims_success_and_keeps_partial_output(self):
        owner=pwd.getpwuid(os.getuid()); real_popen=subprocess.Popen
        def spawn(args,**kwargs):
            for key in ('user','group','extra_groups'): kwargs.pop(key,None)
            if args[:2]==['borg','export-tar']:
                args=[sys.executable,'-c','import sys; sys.stderr.write("forced export failure"); sys.exit(2)']
            return real_popen(args,**kwargs)
        request=dict(command='recover',path=file_recovery.token(self.parent),items=[file_recovery.token(self.parent+b'/one.txt')],destination=str(self.root/'out'))
        with patch.object(file_recovery.subprocess,'Popen',side_effect=spawn):
            result=file_recovery.recover(self.adapter,self.config,self.archive_id,self.db,request,owner,notify=lambda *a,**k:None)
        self.assertFalse(result['ok']); self.assertTrue(Path(result['path']).is_dir())
        self.assertIn('forced export failure',result['details'])
        self.assertIn('incomplete',result['message'])

    def test_mount_mismatch_and_changed_plan_do_not_export(self):
        self.adapter.run=lambda *args,**kwargs:'wrong-uuid'
        request=dict(command='recover',path=file_recovery.token(self.parent),items=[file_recovery.token(self.parent+b'/one.txt')],destination=str(self.root/'out'))
        with patch.object(file_recovery.subprocess,'Popen') as popen:
            with self.assertRaisesRegex(ValueError,'filesystem'): file_recovery.recover(self.adapter,self.config,self.archive_id,self.db,request,pwd.getpwuid(os.getuid()))
            popen.assert_not_called()
        self.adapter.read_config=lambda:dict(self.config,revision='changed')
        with self.assertRaisesRegex(ValueError,'plan changed'): file_recovery.recover(self.adapter,self.config,self.archive_id,self.db,request,pwd.getpwuid(os.getuid()))

    def test_safe_extraction_blocks_escape_links_and_special_files(self):
        stream=io.BytesIO()
        with tarfile.open(fileobj=stream,mode='w') as archive:
            good=tarfile.TarInfo('good.txt'); good.size=4; good.mode=0o6755; good.uid=0; archive.addfile(good,io.BytesIO(b'good'))
            escape=tarfile.TarInfo('../escaped.txt'); escape.size=3; archive.addfile(escape,io.BytesIO(b'bad'))
            link=tarfile.TarInfo('outside'); link.type=tarfile.SYMTYPE; link.linkname=str(self.root/'outside'); archive.addfile(link)
            hardlink=tarfile.TarInfo('bad-hardlink'); hardlink.type=tarfile.LNKTYPE; hardlink.linkname='/etc/passwd'; archive.addfile(hardlink)
            fifo=tarfile.TarInfo('fifo'); fifo.type=tarfile.FIFOTYPE; archive.addfile(fifo)
        stream.seek(0)
        result=file_recovery.extract_stream(str(self.root/'Recovered Files'),'home-test',stream,notify=lambda *a,**k:None)
        self.assertFalse(result['ok']); self.assertEqual(result['skipped'],4)
        output=Path(result['path']); self.assertEqual((output/'good.txt').read_bytes(),b'good')
        self.assertFalse((output.parent/'escaped.txt').exists()); self.assertFalse((self.root/'outside').exists())
        self.assertFalse((output/'outside').exists()); self.assertFalse((output/'fifo').exists())
        self.assertEqual((output/'good.txt').stat().st_mode & 0o6000,0)

    @unittest.skipIf(os.geteuid()==0,'Browser sessions require a non-root desktop user.')
    def test_one_browser_session_can_navigate_and_recover_without_reauthentication(self):
        self.adapter.verification=types.SimpleNamespace(ensure_hook=lambda adapter:None)
        events=[]
        commands=[dict(command='browse',path=file_recovery.token(self.parent),offset=0,search='one.txt'),
                  dict(command='recover',path=file_recovery.token(self.parent),items=[file_recovery.token(self.parent+b'/one.txt')],destination=str(self.root/'Recovered Files'))]
        self.adapter.ENV=self.env
        stdin=io.StringIO(''.join(json.dumps(command)+'\n' for command in commands))
        fake=dict(event='result',ok=True,path=str(self.root/'Recovered Files'/'new'),message='Recovered')
        with patch.dict(os.environ,{'PKEXEC_UID':str(os.getuid())}),patch.object(sys,'stdin',stdin),             patch.object(file_recovery,'send',side_effect=lambda event,**values:events.append(dict(event=event,**values))),             patch.object(file_recovery,'recover',return_value=fake) as recover:
            result=file_recovery.session(self.adapter,dict(archive_id=self.archive_id,revision=self.config['revision']))
        self.assertEqual(result['event'],'closed')
        self.assertEqual([e['event'] for e in events],['ready','listing','result'])
        self.assertEqual(events[0]['destination'],str(Path.home()/'Recovered Files'))
        self.assertEqual(events[1]['total'],1); recover.assert_called_once()

    def test_cached_large_folders_are_paged_and_filter_characters_are_literal(self):
        from borg.item import Item
        class Archive:
            def iter_items(self):
                for i in range(1001): yield Item(path='large/file{:04d}'.format(i),mode=stat.S_IFREG|0o600,size=1,mtime=time.time_ns())
                yield Item(path='large/literal%_[x]',mode=stat.S_IFREG|0o600,size=1,mtime=time.time_ns())
        db=file_recovery.catalogue(self.config,'b'*64,Archive(),lambda message:None)
        try:
            self.assertEqual(len(file_recovery.browse(db,b'large')['entries']),500)
            self.assertEqual(file_recovery.browse(db,b'large',offset=1000)['total'],1002)
            self.assertEqual(file_recovery.browse(db,b'large',search='%_')['total'],1)
        finally: db.close()
        broken=types.SimpleNamespace(iter_items=lambda: (_ for _ in ()).throw(AssertionError('cached catalogue was rebuilt')))
        db=file_recovery.catalogue(self.config,'b'*64,broken,lambda message:None); db.close()


if __name__=='__main__': unittest.main()
