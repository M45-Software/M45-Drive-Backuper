"""Snapshot browsing and isolated, user-owned file recovery."""
import base64
import contextlib
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import sqlite3
import stat
import subprocess
import sys
import tarfile
import tempfile
import time

CACHE = Path('/var/cache/m45-drive-backuper-browser')
ACTIVITY = Path('/var/lib/m45-drive-backuper/file-recovery-activity.json')
BORG_CACHE = Path('/var/cache/local-home-backup')
BORG_SECURITY = Path('/var/lib/local-home-backup/security')
PAGE_SIZE = 500
MAX_COMMAND = 512 * 1024


def send(event, **values):
    print(json.dumps(dict(event=event, **values)), flush=True)


def token(path):
    return base64.b64encode(path).decode('ascii')


def raw_path(value):
    if not isinstance(value, str) or len(value) > 6000:
        raise ValueError('Invalid saved path.')
    path = base64.b64decode(value, validate=True)
    if (len(path) > 4096 or b'\0' in path or path.startswith(b'/') or
            any(p in (b'', b'.', b'..') for p in path.split(b'/')) and path):
        raise ValueError('Invalid saved path.')
    return path


def display(value):
    text = value.decode('utf-8', 'backslashreplace')
    return ''.join('\\x%02x' % ord(c) if ord(c) < 32 or ord(c) == 127 else c for c in text)


def process_start(pid):
    try:
        return Path('/proc/'+str(pid)+'/stat').read_text().rpartition(')')[2].split()[19]
    except (OSError, IndexError):
        return None


def status(config):
    try:
        value=json.loads(ACTIVITY.read_text())
        identity={key:config[key] for key in ('repository','uuid','source')}
        value['active']=bool(value.get('active') and value.get('identity')==identity and
                             value.get('start_ticks') is not None and process_start(value.get('pid'))==value['start_ticks'])
        return value
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return dict(active=False)


@contextlib.contextmanager
def activity(adapter,config,operation):
    value=dict(active=True,operation=operation,pid=os.getpid(),start_ticks=process_start(os.getpid()),
               identity={key:config[key] for key in ('repository','uuid','source')})
    def save():
        try: adapter.atomic(ACTIVITY,json.dumps(value)+'\n',0o644)
        except OSError: pass  # Indicator persistence must not prevent file recovery.
    save()
    try: yield
    finally:
        value['active']=False; save()


def borg_env(adapter):
    env = dict(adapter.ENV, BORG_CACHE_DIR=str(BORG_CACHE), BORG_SECURITY_DIR=str(BORG_SECURITY),
               BORG_UNKNOWN_UNENCRYPTED_REPO_ACCESS_IS_OK='yes')
    if not any(key in env for key in ('BORG_PASSPHRASE','BORG_PASSCOMMAND','BORG_PASSPHRASE_FD')):
        env['BORG_PASSPHRASE'] = ''  # Fail rather than consume the browser protocol as a passphrase.
    return env


@contextlib.contextmanager
def job_lock(adapter):
    with adapter.LOCK.open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError('A backup, check or recovery is running. Try again when it finishes.') from None
        yield


def destination_ready(adapter, config):
    if config.get('auto_mount'):
        adapter.ensure_mount(config['mountpoint'], config['uuid'])
    if adapter.run('findmnt', '-rn', '-M', config['mountpoint'], '-o', 'UUID', check=False) != config['uuid']:
        raise ValueError('The expected backup filesystem is not mounted.')


@contextlib.contextmanager
def saved_archive(config, archive_id):
    import borg.logger
    if not borg.logger.configured:
        borg.logger.setup_logging(stream=sys.stderr, env_var=None, level='warning')
    from borg.repository import Repository
    from borg.helpers import Manifest
    from borg.archive import Archive
    if not isinstance(archive_id, str) or (archive_id != 'latest' and not re.fullmatch('[a-f0-9]{64}', archive_id)):
        raise ValueError('Select a saved snapshot from the refreshed backup list.')
    no_credential = not any(key in os.environ for key in ('BORG_PASSPHRASE','BORG_PASSCOMMAND','BORG_PASSPHRASE_FD'))
    if no_credential: os.environ['BORG_PASSPHRASE'] = ''
    try:
        with Repository(config['repository'], exclusive=False, lock_wait=5) as repository:
            manifest, key = Manifest.load(repository, (Manifest.Operation.READ,))
            candidates = [a for a in manifest.archives.list() if a.name.startswith('home-')]
            if archive_id == 'latest':
                completed = [a for a in candidates if not re.search(r'\.checkpoint(?:\.\d+)?$', a.name)]
                matches = [max(completed,key=lambda a:a.ts)] if completed else []
            else:
                matches = [a for a in candidates if a.id.hex() == archive_id]
            if not matches:
                raise ValueError('This snapshot is no longer available. Refresh the backup list.')
            if re.search(r'\.checkpoint(?:\.\d+)?$', matches[0].name):
                raise ValueError('Choose a completed snapshot rather than an incomplete checkpoint.')
            archive = Archive(repository, key, manifest, matches[0].name)
            yield archive
    finally:
        if no_credential: os.environ.pop('BORG_PASSPHRASE',None)


def catalogue(config, archive_id, archive, progress):
    CACHE.mkdir(mode=0o700, parents=True, exist_ok=True)
    CACHE.chmod(0o700)
    repo_key = hashlib.sha256((config['repository']+'\0'+config['uuid']).encode()).hexdigest()[:24]
    target = CACHE / ('v1-'+repo_key+'-'+archive_id+'.sqlite')
    if not target.exists():
        progress('Reading snapshot metadata. This first browse may take a minute.')
        with tempfile.TemporaryDirectory(prefix='.build-', dir=CACHE) as tmp:
            temporary = Path(tmp)/'catalogue.sqlite'
            db = sqlite3.connect(temporary)
            try:
                db.execute('PRAGMA journal_mode=OFF'); db.execute('PRAGMA synchronous=OFF')
                db.execute('CREATE TABLE entries(path BLOB PRIMARY KEY,parent BLOB,name TEXT,kind TEXT,size INTEGER,mtime TEXT,source BLOB,explicit INTEGER) WITHOUT ROWID')
                db.execute('INSERT INTO entries VALUES(?,?,?,?,?,?,?,?)',(b'',None,'','d',0,'',None,0))
                directories = {b''}; batch = []; count = 0
                sql = ('INSERT INTO entries VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(path) DO UPDATE SET '
                       'parent=excluded.parent,name=excluded.name,kind=excluded.kind,size=excluded.size,'
                       'mtime=excluded.mtime,source=excluded.source,explicit=excluded.explicit WHERE excluded.explicit=1')
                for item in archive.iter_items():
                    path = os.fsencode(item.path)
                    if path in (b'',b'.'): continue
                    raw_path(token(path))
                    parts = path.split(b'/')
                    for n in range(1,len(parts)):
                        parent = b'/'.join(parts[:n])
                        if parent not in directories:
                            batch.append((parent,parent.rpartition(b'/')[0],display(parts[n-1]),'d',0,'',None,0))
                            directories.add(parent)
                    kind = 'd' if stat.S_ISDIR(item.mode) else 'f' if stat.S_ISREG(item.mode) else 'l' if stat.S_ISLNK(item.mode) else 'special'
                    if kind == 'd': directories.add(path)
                    source = os.fsencode(item.source) if kind == 'f' and 'source' in item else None
                    size = item.get('size') if kind == 'f' else 0
                    if size is None and kind == 'f' and 'chunks' in item: size = item.get_size()
                    mtime = datetime.datetime.fromtimestamp(item.mtime/1e9,datetime.timezone.utc).isoformat()
                    batch.append((path,path.rpartition(b'/')[0],display(parts[-1]),kind,size,mtime,source,1)); count += 1
                    if len(batch) >= 2000:
                        db.executemany(sql,batch); batch.clear()
                    if count % 50000 == 0: progress('Indexed {:,} saved items…'.format(count))
                if batch: db.executemany(sql,batch)
                db.execute('UPDATE entries SET size=(SELECT master.size FROM entries AS master WHERE master.path=entries.source) WHERE kind=\'f\' AND size IS NULL')
                db.execute('CREATE INDEX children ON entries(parent,kind,name)')
                db.commit()
            finally:
                db.close()
            temporary.chmod(0o600)
            os.replace(temporary,target)
    target.touch()
    # These are disposable metadata indexes, never backup data.
    for old in sorted(CACHE.glob('v1-*.sqlite'),key=lambda p:p.stat().st_mtime,reverse=True)[3:]:
        if old != target: old.unlink(missing_ok=True)
    db = sqlite3.connect('file:'+str(target)+'?mode=ro&immutable=1',uri=True)
    db.row_factory = sqlite3.Row
    return db


def browse(db, path=b'', offset=0, search=''):
    if type(offset) is not int or not 0 <= offset <= 10000000 or not isinstance(search,str) or len(search)>200:
        raise ValueError('Invalid browse request.')
    row = db.execute('SELECT kind FROM entries WHERE path=?',(path,)).fetchone()
    if not row or row['kind'] != 'd': raise ValueError('This saved folder is unavailable.')
    escaped = search.replace('\\','\\\\').replace('%','\\%').replace('_','\\_')
    where = "parent=? AND name LIKE ? ESCAPE '\\'"
    args = (path,'%'+escaped+'%')
    total = db.execute('SELECT COUNT(*) FROM entries WHERE '+where,args).fetchone()[0]
    rows = db.execute('SELECT * FROM entries WHERE '+where+' ORDER BY (kind=\'d\') DESC,name COLLATE NOCASE,path LIMIT ? OFFSET ?',(*args,PAGE_SIZE,offset)).fetchall()
    return dict(path=token(path),parent=token(path.rpartition(b'/')[0]),display_path='/'+display(path),offset=offset,total=total,
                entries=[dict(token=token(r['path']),name=r['name'],kind=r['kind'],size=r['size'] or 0,mtime=r['mtime']) for r in rows])


def selected_paths(db, parent, encoded):
    if not isinstance(encoded,list) or not 1 <= len(encoded) <= 200:
        raise ValueError('Select up to 200 items at once, or choose a parent folder to recover more.')
    paths = list(dict.fromkeys(raw_path(x) for x in encoded))
    if sum(len(p) for p in paths)>100000: raise ValueError('This selection is too large. Choose a parent folder instead.')
    for path in paths:
        row = db.execute('SELECT parent,kind FROM entries WHERE path=?',(path,)).fetchone()
        if not row or row['parent'] != parent or row['kind'] not in ('d','f','l'):
            raise ValueError('Select regular files, links or folders from the current saved folder.')
    return paths


def extract_stream(destination, label, stream, notify=send):
    """Runs only as the desktop user, never as root in the production path."""
    if not hasattr(tarfile,'data_filter'): raise ValueError('Safe tar extraction requires an updated Python installation.')
    base = Path(destination)
    if not base.is_absolute() or '\0' in destination or len(destination)>4096:
        raise ValueError('Choose an absolute recovery destination.')
    os.umask(0o077)
    base.mkdir(parents=True,exist_ok=True)
    slug = re.sub('[^A-Za-z0-9_.-]','_',label)[:70].strip('._') or 'backup'
    prefix = slug+'-recovered-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S')+'-'
    output = Path(tempfile.mkdtemp(prefix=prefix,dir=base))
    notify('destination',path=str(output))
    count=0; skipped=0; warnings=[]; last=0
    try:
        with tarfile.open(fileobj=stream,mode='r|') as archive:
            for member in archive:
                try:
                    if not (member.isfile() or member.isdir() or member.issym() or member.islnk()):
                        raise tarfile.FilterError('Special files are not restored.')
                    archive.extract(member,path=output,filter='data')
                    count += 1
                except (tarfile.FilterError,KeyError) as error:
                    skipped += 1
                    if len(warnings)<30: warnings.append(display(os.fsencode(member.name))+': '+str(error))
                if time.monotonic()-last>1:
                    notify('progress',message='Recovered {:,} items…'.format(count)); last=time.monotonic()
        result=dict(event='result',path=str(output),count=count,skipped=skipped,warnings=warnings,ok=not skipped and count>0)
    except Exception as error:
        result=dict(event='result',path=str(output),count=count,skipped=skipped,warnings=warnings,ok=False,error=str(error))
    return result


def extract_main(data):
    if os.geteuid()==0: raise PermissionError('Recovery extraction must run as the desktop user.')
    if not isinstance(data,dict) or set(data)!={'destination','label'}: raise ValueError('Invalid recovery destination.')
    return extract_stream(data['destination'],data['label'],sys.stdin.buffer)


def recover(adapter, config, archive_id, db, data, owner, notify=send):
    if set(data)!={'command','path','items','destination'}: raise ValueError('Invalid recovery request.')
    parent=raw_path(data['path']); paths=selected_paths(db,parent,data['items'])
    destination=data['destination']
    if not isinstance(destination,str) or not Path(destination).is_absolute() or len(destination)>4096 or '\0' in destination:
        raise ValueError('Choose an absolute recovery destination.')
    if Path(destination).resolve().is_relative_to(Path(config['repository']).resolve()):
        raise ValueError('Recover files outside the Borg repository.')
    with job_lock(adapter), activity(adapter,config,'recover'):
        current=adapter.read_config()
        if current['revision']!=config['revision']:
            raise ValueError('The backup plan changed. Reopen the snapshot browser before recovering.')
        destination_ready(adapter,config)
        with saved_archive(config,archive_id) as archive: name=archive.name
        command=['borg','export-tar','--strip-components',str(len(parent.split(b'/')) if parent else 0),
                 config['repository']+'::'+name,'-',*['pp:'+os.fsdecode(path) for path in paths]]
        payload=base64.b64encode(json.dumps(dict(destination=destination,label=name)).encode()).decode()
        env=dict(PATH='/usr/sbin:/usr/bin:/sbin:/bin',HOME=owner.pw_dir,USER=owner.pw_name,LOGNAME=owner.pw_name,LANG='C.UTF-8',LC_ALL='C.UTF-8')
        with tempfile.TemporaryFile() as worker_errors,tempfile.TemporaryFile() as export_errors:
            worker=None; exporter=None; result=None
            try:
                worker=subprocess.Popen([sys.executable,str(Path(adapter.__file__).resolve()),'recovery-extract',payload],
                    stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=worker_errors,text=False,env=env,
                    user=owner.pw_uid,group=owner.pw_gid,extra_groups=os.getgrouplist(owner.pw_name,owner.pw_gid))
                first=json.loads(worker.stdout.readline())
                if first.get('event')!='destination': raise ValueError(first.get('error','Could not create a recovery folder.'))
                notify('destination',path=first['path'])
                exporter=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=worker.stdin,stderr=export_errors,env=borg_env(adapter))
                worker.stdin.close(); worker.stdin=None
                for line in worker.stdout:
                    event=json.loads(line)
                    if event.get('event')=='result': result=event
                    else: notify(event.pop('event'),**event)
                worker_rc=worker.wait(); export_rc=exporter.wait(timeout=30)
                if not result: result=dict(event='result',path=first['path'],ok=False,error='Recovery stopped before completion.')
                export_errors.seek(0,2); length=export_errors.tell(); export_errors.seek(max(0,length-16000))
                details=export_errors.read().decode(errors='replace')
                result['ok']=result.get('ok',False) and worker_rc==0 and export_rc==0
                if details: result['details']=details
                if not result['ok']: result['message']='Recovery was incomplete. Partial files are kept in the new recovery folder.'
                else: result['message']='Selected items recovered successfully.'
                return result
            finally:
                for process in (worker,exporter):
                    if process:
                        if process.poll() is None:
                            process.terminate()
                            try: process.wait(timeout=10)
                            except subprocess.TimeoutExpired: process.kill(); process.wait()
                        for pipe in (process.stdin,process.stdout):
                            if pipe: pipe.close()


def session(adapter, data):
    if not isinstance(data,dict) or set(data)!={'archive_id','revision'}: raise ValueError('Invalid snapshot selection.')
    uid=int(os.environ.get('PKEXEC_UID','0'))
    if uid<=0: raise PermissionError('Open file recovery from a non-root desktop session.')
    owner=pwd.getpwuid(uid)
    config=adapter.read_config()
    if data['revision']!=config['revision']: raise ValueError('The backup plan changed. Refresh before opening the browser.')
    with job_lock(adapter), activity(adapter,config,'browse'):
        destination_ready(adapter,config)
        adapter.verification.ensure_hook(adapter)
        config=adapter.read_config()
        with saved_archive(config,data['archive_id']) as archive:
            name=archive.name; archive_id=archive.id.hex()
            db=catalogue(config,archive_id,archive,lambda message:send('progress',message=message))
    try:
        source=os.fsencode(config['source'].lstrip('/'))
        initial=source if db.execute('SELECT 1 FROM entries WHERE path=? AND kind=\'d\'',(source,)).fetchone() else b''
        send('ready',archive=name,destination=str(Path(owner.pw_dir)/'Recovered Files'),**browse(db,initial))
        while True:
            line=sys.stdin.readline(MAX_COMMAND+1)
            if not line: break
            if len(line)>MAX_COMMAND: raise ValueError('Recovery request is too large.')
            try:
                command=json.loads(line)
                if not isinstance(command,dict): raise ValueError('Invalid browser request.')
                if command.get('command')=='browse' and set(command)=={'command','path','offset','search'}:
                    send('listing',**browse(db,raw_path(command['path']),command['offset'],command['search']))
                elif command.get('command')=='recover':
                    result=recover(adapter,config,archive_id,db,command,owner)
                    send(result.pop('event'),**result)
                else: raise ValueError('Unsupported browser action.')
            except Exception as error:
                send('error',message=str(error))
    finally:
        db.close()
    return dict(event='closed')
