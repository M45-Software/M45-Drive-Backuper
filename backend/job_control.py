"""Fixed-purpose pause/cancel/one-time postponement for the two managed jobs."""
import contextlib
import fcntl
import json
from pathlib import Path
import time
import idle_policy

STATE = Path('/var/lib/m45-drive-backuper/job-controls.json')
LOCK = Path('/run/lock/m45-job-control.lock')
CGROUP = Path('/sys/fs/cgroup/system.slice')
UNITS = {'backup': 'local-system-backup.service', 'check': 'm45-backup-check.service'}
ACTIVE = ('active', 'activating', 'deactivating')


def read():
    try:
        value = json.loads(STATE.read_text())
        if not isinstance(value, dict): raise ValueError('Invalid job-control state.')
        return value
    except FileNotFoundError:
        return {}


def freezer(kind):
    # No user-provided paths, PIDs, unit names or shell commands.
    return CGROUP / UNITS[kind] / 'cgroup.freeze'


def status():
    try:
        state = read()
        result = {}
        for kind in UNITS:
            entry = dict(state.get(kind, {}))
            entry['cancelled_invocation'] = state.get('_cancelled', {}).get(kind, '')
            try:
                entry['paused'] = freezer(kind).read_text().strip() == '1'
                entry['can_pause'] = True
            except OSError:
                entry.update(paused=False, can_pause=False)
            result[kind] = entry
        return result
    except (OSError, ValueError, TypeError) as error:
        return {'error': str(error)}


def allowed(kind, manual=False):
    if kind not in UNITS: raise ValueError('Unknown job.')
    entry = read().get(kind)
    return not entry or (manual and entry.get("mode") == "idle_wait")


@contextlib.contextmanager
def locked():
    with LOCK.open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        yield


def save(adapter, state):
    adapter.atomic(STATE, json.dumps(state)+'\n', 0o644)


def thaw(kind):
    try:
        freezer(kind).write_text('0')
    except FileNotFoundError:
        pass


def refresh_selection(adapter, entry):
    # Pausing before a queued selection is consumed must not expire that
    # selection solely because the user deliberately waited over an hour.
    path = adapter.verification.REQUEST
    if not path.exists(): return
    pending = json.loads(path.read_text())
    if pending.get('revision') == entry.get('revision') == adapter.read_config()['revision']:
        pending['requested_at'] = time.time()
        adapter.atomic(path,json.dumps(pending)+'\n',0o600)


def release(adapter, state, kind, run_pending):
    entry = state.get(kind, {})
    props = adapter.properties(UNITS[kind])
    if entry.get('invocation'):
        # A reboot or a replacement job must never resurrect/resume the old job.
        if props.get('InvocationID') == entry['invocation'] and props.get('ActiveState') in ACTIVE:
            if kind == 'check':
                refresh_selection(adapter, entry)
            thaw(kind)
    elif run_pending and entry.get('mode') in ('postponed','idle_wait'):
        config = adapter.read_config()
        if config['revision'] == entry.get('revision'):
            state.pop(kind, None)
            bypass_once(state,kind,config)
            save(adapter, state)  # ExecCondition must see the released gate.
            try:
                adapter.run('systemctl', 'start', '--no-block', UNITS[kind])
            except Exception:
                state[kind] = entry
                save(adapter, state)
                raise
            return
    state.pop(kind, None)
    save(adapter, state)


def tick(adapter, now=None):
    now = time.time() if now is None else now
    with locked():
        state = read()
        for kind in UNITS:
            entry = state.get(kind, {})
            if entry.get('mode') == 'idle_wait':
                config = adapter.read_config()
                if config['revision'] != entry.get('revision'):
                    state.pop(kind,None); save(adapter,state); continue
                if kind == 'check' and entry.get('check_revision') != adapter.verification.revision(config):
                    state.pop(kind,None); save(adapter,state); continue
                idle,_,available = idle_policy.desktop_idle(adapter)
                if idle or not available or entry['until'] <= now or not idle_policy.settings()[kind]:
                    release(adapter,state,kind,True)
            elif entry.get('until', 0) and entry['until'] <= now:
                release(adapter, state, kind, True)
    return dict(message='Due postponements processed.')


def control(adapter, data):
    if (not isinstance(data, dict) or set(data) != {'kind','operation','delay','revision','invocation'} or
            data['kind'] not in UNITS or data['operation'] not in ('pause','resume','cancel','postpone') or
            type(data['delay']) is not int or data['delay'] not in (0,3600,14400,86400) or
            (data['operation'] == 'postpone') != bool(data['delay']) or
            not isinstance(data['invocation'], str) or not isinstance(data['revision'], str)):
        raise ValueError('Invalid job control request.')
    kind, operation = data['kind'], data['operation']
    with locked():
        config = adapter.read_config()
        props = adapter.properties(UNITS[kind])
        active = props.get('ActiveState') in ACTIVE
        invocation = props.get('InvocationID', '') if active else ''
        if config['revision'] != data['revision'] or invocation != data['invocation']:
            raise ValueError('The job or backup plan changed. Refresh and try again.')
        state = read()
        if operation == 'cancel':
            # Stop first so termination is queued before a frozen process runs.
            adapter.run('systemctl', 'stop', '--no-block', UNITS[kind])
            thaw(kind)
            if kind == 'check': adapter.verification.REQUEST.unlink(missing_ok=True)
            state.pop(kind, None)
            if invocation: state.setdefault('_cancelled', {})[kind] = invocation
            save(adapter, state)
            return dict(message='Cancellation requested. Partial runs do not count as successful. The normal schedule remains unchanged.')
        if operation == 'resume':
            release(adapter, state, kind, True)
            return dict(message='Job resumed or temporary schedule pause cleared.')
        if props.get('ActiveState') == 'deactivating':
            raise ValueError('The job is already stopping. Wait for it to finish.')
        # Enable only on explicit use; no housekeeping timer is enabled by install.
        adapter.run('systemctl', 'enable', '--now', 'm45-job-control.timer')
        previous = state.get(kind)
        entry = dict(mode='postponed' if operation == 'postpone' else 'paused',
                     until=time.time()+data['delay'] if data['delay'] else 0,
                     invocation=invocation, revision=config['revision'])
        state[kind] = entry
        save(adapter, state)  # Gate future timer/after-backup requests first.
        if kind == 'check' and not active: adapter.verification.REQUEST.unlink(missing_ok=True)
        was_frozen = active and freezer(kind).exists() and freezer(kind).read_text().strip() == '1'
        try:
            if active:
                freezer(kind).write_text('1')
                # Wait briefly for the kernel to confirm the whole job is frozen.
                events = freezer(kind).with_name('cgroup.events')
                for _ in range(100):
                    if 'frozen 1' in events.read_text().splitlines(): break
                    time.sleep(.02)
                else: raise RuntimeError('The job could not pause yet. Try again shortly.')
        except Exception:
            if was_frozen: freezer(kind).write_text('1')
            else: thaw(kind)
            if previous is None: state.pop(kind, None)
            else: state[kind] = previous
            save(adapter, state)
            raise
        return dict(message=('Job paused. Resume or cancel it here; it keeps its repository lock and requires the drive to stay connected.' if active else 'Future runs paused. Resume restores the normal schedule.') if operation == 'pause' else 'Postponed. The job will resume, or a new run will start, at the displayed time even with the app closed.')


def bypass_once(state, kind, config):
    if idle_policy.settings()[kind]:
        state.setdefault('_bypass',{})[kind] = dict(revision=config['revision'],expires=time.time()+120)


def manual_start(adapter, kind):
    if not idle_policy.settings()[kind] and not read().get(kind): return
    with locked():
        state = read()
        if state.get(kind,{}).get('mode') == 'idle_wait': state.pop(kind)
        if kind in state: raise ValueError('The job is paused or postponed. Resume it first.')
        bypass_once(state,kind,adapter.read_config())
        save(adapter,state)


def gate(adapter, kind, now=None):
    if kind not in UNITS: raise ValueError('Unknown job.')
    now = time.time() if now is None else now
    with locked():
        state = read()
        if kind in state: return False  # Repeated triggers must not extend a deadline.
        config = adapter.read_config()
        bypass = state.get('_bypass',{}).pop(kind,{})
        if bypass:
            save(adapter,state)
            if bypass.get('revision') == config['revision'] and now <= bypass.get('expires',0): return True
        options = idle_policy.settings()
        if not options[kind]: return True
        idle,reason,available = idle_policy.desktop_idle(adapter)
        if idle or not available: return True
        entry = dict(mode='idle_wait',queued_at=now,until=now+options['max_hours']*3600,
                     invocation='',revision=config['revision'],idle_reason=reason)
        if kind == 'check': entry['check_revision'] = adapter.verification.revision(config)
        state[kind] = entry
        save(adapter,state)
        return False


def discard_idle(adapter,kind):
    if read().get(kind,{}).get('mode') != 'idle_wait': return
    with locked():
        state=read()
        if state.get(kind,{}).get('mode') == 'idle_wait':
            state.pop(kind); save(adapter,state)
