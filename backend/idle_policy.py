"""Optional, bounded deferral of automatic jobs using logind idle hints."""
import hashlib
import json
from pathlib import Path
import re
import time
import subprocess
from types import SimpleNamespace

SETTINGS = Path('/etc/m45-drive-backuper-idle.json')
DEFAULTS = dict(backup=False, check=False, max_hours=6)
IDLE_SECONDS = 10 * 60


def settings():
    try:
        value = json.loads(SETTINGS.read_text())
    except FileNotFoundError:
        return dict(DEFAULTS)
    if (not isinstance(value, dict) or set(value) != set(DEFAULTS) or
            type(value['backup']) is not bool or type(value['check']) is not bool or
            type(value['max_hours']) is not int or value['max_hours'] not in (6,12,24,48)):
        raise ValueError('Invalid idle-wait settings.')
    return value


def revision(value):
    return hashlib.sha256(json.dumps(value,sort_keys=True).encode()).hexdigest()


def desktop_idle(adapter, monotonic=None):
    """All user sessions must report idle for ten minutes. Unsupported reporting disables deferral."""
    monotonic = time.monotonic() if monotonic is None else monotonic
    try:
        deadline = time.monotonic()+8
        listing = adapter.run('loginctl','list-sessions','--no-legend','--no-pager',timeout=3)
        ids = [line.split()[0] for line in listing.splitlines() if line.strip()]
        if len(ids)>64 or any(not re.fullmatch(r'[A-Za-z0-9_-]{1,64}',sid) for sid in ids):
            raise ValueError('Unexpected session list.')
        busy = False
        for sid in ids:
            remaining = deadline-time.monotonic()
            if remaining <= 0: raise RuntimeError('Session queries took too long.')
            output = adapter.run('loginctl','show-session',sid,'--no-pager',
                '-p','Type','-p','Class','-p','IdleHint','-p','IdleSinceHintMonotonic',timeout=min(2,remaining))
            props = dict(line.split('=',1) for line in output.splitlines() if '=' in line)
            if props.get('Class') in ('greeter','manager','manager-early','background'): continue
            if props.get('Class') not in ('user','user-early') or props.get('Type') not in ('x11','wayland','tty'):
                return False, 'Idle reporting is unavailable for a login session.', False
            if props.get('IdleHint') not in ('yes','no'):
                return False, 'The login session does not provide idle hints.', False
            since = int(props.get('IdleSinceHintMonotonic','0')) / 1_000_000
            if props['IdleHint'] == 'no': busy = True
            elif since <= 0 or since > monotonic:
                return False, 'The login session does not provide a valid idle timestamp.', False
            elif monotonic-since < IDLE_SECONDS: busy = True
        return (False, 'A user session is active or has not reported idle for 10 minutes.', True) if busy else (True, 'All user sessions report idle for 10 minutes, or no user is logged in.', True)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        return False, 'Idle reporting unavailable: ' + str(error)[:200], False


def status(run):
    try:
        value = settings()
        result = dict(value,revision=revision(value))
        result['idle'],result['message'],result['available'] = desktop_idle(SimpleNamespace(run=run))
        if not result['available']: result['message'] += ' Idle waiting is disabled; normal schedules apply.'
        elif not value['backup'] and not value['check']: result['message'] = 'Idle waiting is off. Idle reporting is available.'
        return result
    except (OSError, ValueError) as error:
        return dict(DEFAULTS,available=False,error=str(error),message='Idle-wait settings could not be read.')


def configure(adapter, data):
    import job_control
    if (not isinstance(data,dict) or set(data) != set(DEFAULTS)|{'revision'} or
            type(data['backup']) is not bool or type(data['check']) is not bool or
            type(data['max_hours']) is not int or data['max_hours'] not in (6,12,24,48)):
        raise ValueError('Invalid idle-wait options.')
    with job_control.locked():
        if data['revision'] != revision(settings()):
            raise ValueError('Idle-wait options changed. Refresh before saving.')
        value = {key:data[key] for key in DEFAULTS}
        if value['backup'] or value['check']:
            _,reason,available = desktop_idle(adapter)
            if not available: raise ValueError(reason + ' Idle waiting cannot be enabled on this system.')
        if value['backup'] or value['check']:
            adapter.run('systemctl','enable','--now','m45-job-control.timer')
        old = settings()
        adapter.atomic(SETTINGS,json.dumps(value)+'\n',0o644)
        state = job_control.read()
        for kind in job_control.UNITS:
            entry = state.get(kind,{})
            if entry.get('mode') != 'idle_wait': continue
            if not value[kind]: job_control.release(adapter,state,kind,True)
            else:
                started = entry.get('queued_at',entry['until']-old['max_hours']*3600)
                entry['until'] = min(entry['until'], started+value['max_hours']*3600)
                job_control.save(adapter,state)
    return dict(message='Idle-wait options saved. Automatic jobs start when idle or when the maximum delay expires; manual runs start immediately.')
