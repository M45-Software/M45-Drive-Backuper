#!/usr/bin/python3
"""Root-only smoke test using disposable services; never touches real jobs."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import types
import uuid

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'backend'))
import job_control as jobs
import idle_policy


def main():
    if os.geteuid()!=0: raise SystemExit('Run as root to test disposable systemd services.')
    root=Path(tempfile.mkdtemp(prefix='m45-control-smoke-',dir='/var/tmp'))
    prefix='m45-control-smoke-'+uuid.uuid4().hex[:10]
    units={kind:prefix+'-'+kind+'.service' for kind in ('backup','check')}
    jobs.UNITS=units; jobs.STATE=root/'state.json'; jobs.LOCK=root/'lock'
    request=root/'request.json'
    idle_policy.SETTINGS=root/'idle-policy.json'
    idle_input=root/'idle-input'; idle_input.write_text('busy')
    def probe(*args,**kwargs):
        value=idle_input.read_text()
        return value=='idle','fixture idle state',value!='unavailable'
    idle_policy.desktop_idle=probe
    gate_script=root/'gate.py'
    gate_script.write_text(f"""import sys,types
from pathlib import Path
sys.path.insert(0,{str(Path(__file__).resolve().parents[1]/'backend')!r})
import job_control as jobs
import idle_policy
jobs.UNITS={units!r}
jobs.STATE=Path({str(jobs.STATE)!r}); jobs.LOCK=Path({str(jobs.LOCK)!r})
idle_policy.SETTINGS=Path({str(idle_policy.SETTINGS)!r})
def probe(*a,**kw):
 value=Path({str(idle_input)!r}).read_text()
 return value=='idle','fixture idle state',value!='unavailable'
idle_policy.desktop_idle=probe
def atomic(path,value,mode):
 temporary=path.with_name(path.name+'.new'); temporary.write_text(value); temporary.chmod(mode); temporary.replace(path)
adapter=types.SimpleNamespace(atomic=atomic,read_config=lambda:dict(revision='a'*64),verification=types.SimpleNamespace(revision=lambda c:'fixture-check-revision'))
sys.exit(0 if jobs.gate(adapter,sys.argv[1]) else 1)
""")
    script=root/'heartbeat.py'
    # The child loop is deliberately independent: freezing must stop both.
    script.write_text("""import subprocess,sys,time
from pathlib import Path
p=Path(sys.argv[1])
if len(sys.argv)<3:
 subprocess.Popen([sys.executable,__file__,str(p)+'.child','child'])
while True:
 with p.open('a') as f: f.write('x')
 time.sleep(.05)
""")
    def run(*args,**kwargs):
        if args==('systemctl','enable','--now','m45-job-control.timer'): return ''
        assert args[0]=='systemctl' and args[-1] in units.values(),args
        return subprocess.run(args,check=True,capture_output=True,text=True,timeout=15).stdout.strip()
    def props(unit):
        return dict(line.split('=',1) for line in subprocess.run(['systemctl','show',unit,'-p','ActiveState','-p','InvocationID'],check=True,capture_output=True,text=True).stdout.splitlines())
    def atomic(path,value,mode):
        temporary=path.with_name(path.name+'.new'); temporary.write_text(value); temporary.chmod(mode); temporary.replace(path)
    config=dict(revision='a'*64)
    adapter=types.SimpleNamespace(read_config=lambda:config,properties=props,run=run,atomic=atomic,verification=types.SimpleNamespace(REQUEST=request,revision=lambda c:'fixture-check-revision'))
    def data(kind,operation,delay=0):
        p=props(units[kind]); return dict(kind=kind,operation=operation,delay=delay,revision=config['revision'],invocation=p.get('InvocationID','') if p.get('ActiveState') in jobs.ACTIVE else '')
    def wait_for(predicate):
        for _ in range(100):
            if predicate(): return
            time.sleep(.05)
        raise AssertionError('Fixture did not reach expected state')
    paths=[]
    try:
        for kind,unit in units.items():
            path=Path('/run/systemd/system')/unit; paths.append(path)
            path.write_text(f'[Service]\nType=oneshot\nExecCondition=/usr/bin/python3 {gate_script} {kind}\nExecStart=/usr/bin/python3 {script} {root/kind}\nTimeoutStartSec=infinity\nTimeoutStopSec=5s\n')
        subprocess.run(['systemctl','daemon-reload'],check=True)
        for kind in units:
            run('systemctl','start','--no-block',units[kind])
            file=root/kind; child=Path(str(file)+'.child')
            wait_for(lambda:file.exists() and child.exists() and file.stat().st_size>2 and child.stat().st_size>2)
            assert props(units[kind])['ActiveState']=='activating'
            jobs.control(adapter,data(kind,'pause'))
            before=(file.stat().st_size,child.stat().st_size)
            time.sleep(.25)
            assert before==(file.stat().st_size,child.stat().st_size),'A child kept running while paused'
            jobs.control(adapter,data(kind,'resume'))
            wait_for(lambda:file.stat().st_size>before[0] and child.stat().st_size>before[1])
            jobs.control(adapter,data(kind,'postpone',3600))
            before=(file.stat().st_size,child.stat().st_size)
            time.sleep(.15)
            assert before==(file.stat().st_size,child.stat().st_size)
            jobs.tick(adapter,jobs.read()[kind]['until']+1)
            wait_for(lambda:file.stat().st_size>before[0])
            jobs.control(adapter,data(kind,'pause'))
            jobs.control(adapter,data(kind,'cancel'))
            wait_for(lambda:props(units[kind])['ActiveState'] not in jobs.ACTIVE)
            assert jobs.allowed(kind)
            jobs.control(adapter,data(kind,'postpone',3600))
            jobs.tick(adapter,jobs.read()[kind]['until']+1)
            wait_for(lambda:props(units[kind])['ActiveState']=='activating')
            jobs.control(adapter,data(kind,'cancel'))
            wait_for(lambda:props(units[kind])['ActiveState'] not in jobs.ACTIVE)
            print(kind+': actual oneshot + child freeze/resume, timed resume, cancellation while frozen, and one-time start passed',flush=True)
        idle_policy.SETTINGS.write_text(json.dumps(dict(backup=True,check=True,max_hours=6)))
        for kind in units:
            file=root/kind
            for reason in ('idle','deadline','unavailable'):
                idle_input.write_text('busy')
                before=file.stat().st_size
                run('systemctl','start','--no-block',units[kind])
                wait_for(lambda:jobs.read().get(kind,{}).get('mode')=='idle_wait')
                wait_for(lambda:props(units[kind])['ActiveState'] not in jobs.ACTIVE)
                time.sleep(.1); assert before==file.stat().st_size
                entry=jobs.read()[kind]
                assert 21590 < entry['until']-time.time() <= 21600
                if reason=='deadline': jobs.tick(adapter,entry['until']+1)
                else:
                    idle_input.write_text(reason)
                    jobs.tick(adapter)
                wait_for(lambda:file.stat().st_size>before)
                assert kind not in jobs.read()
                jobs.control(adapter,data(kind,'cancel'))
                wait_for(lambda:props(units[kind])['ActiveState'] not in jobs.ACTIVE)
            print(kind+': real ExecCondition held busy starts, then released on idle, six-hour deadline, or unavailable reporting',flush=True)

    finally:
        for kind,unit in units.items():
            try: jobs.thaw(kind)
            except OSError: pass
            subprocess.run(['systemctl','stop',unit],capture_output=True)
            subprocess.run(['systemctl','reset-failed',unit],capture_output=True)
        for path in paths: path.unlink(missing_ok=True)
        subprocess.run(['systemctl','daemon-reload'],check=True)
        shutil.rmtree(root)

if __name__=='__main__': main()
