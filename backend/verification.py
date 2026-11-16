"""Opt-in, serialized Borg integrity checks. Never invoke Borg repair."""
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import signal
import time
import tempfile
import secrets
import job_control

SETTINGS = Path('/etc/m45-drive-backuper-verification.json')
STATE = Path('/var/lib/m45-drive-backuper/verification.json')
REQUEST = Path('/run/m45-drive-backuper/verification-request.json')
TIMER_DROPIN = Path('/etc/systemd/system/m45-backup-check.timer.d/schedule.conf')
SERVICE = 'm45-backup-check.service'
TIMER = 'm45-backup-check.timer'
MODES = {'manual': None, 'after_backup': None, 'daily': '*-*-* 03:00:00',
         'weekly': 'Sun *-*-* 03:00:00', 'monthly': '*-*-01 03:00:00'}


def identity(config):
    return {key: config[key] for key in ('repository', 'uuid', 'source')}


def settings():
    try:
        data = json.loads(SETTINGS.read_text())
    except FileNotFoundError:
        return dict(mode='manual', scope='latest')
    if (not isinstance(data, dict) or set(data) not in ({'mode'}, {'mode', 'scope'}) or
            not isinstance(data['mode'], str) or data['mode'] not in MODES or
            data.get('scope', 'latest') not in ('latest', 'all')):
        raise ValueError('Verification settings are invalid. Save a supported check schedule and scope.')
    return dict(mode=data['mode'], scope=data.get('scope', 'latest'))


def history(config):
    try:
        state = json.loads(STATE.read_text())
        if state.get('identity') == identity(config):
            last = state.get('last_success', {})
            full = state.get('last_full_success', last if last.get('scope', 'all') == 'all' else {})
            return last, full
    except (OSError, ValueError, TypeError, AttributeError):
        pass
    return {}, {}


def revision(config):
    return hashlib.sha256(json.dumps([settings(), config['revision']], sort_keys=True).encode()).hexdigest()


def status(config, properties):
    try:
        saved = settings()
        result = dict(mode=saved['mode'], scope=saved['scope'], revision=revision(config), attempt={}, last_success={}, last_full_success={})
        try:
            state = json.loads(STATE.read_text())
            if state.get('identity') == identity(config):
                last, full = history(config)
                result.update(attempt=state.get('attempt', {}), last_success=last, last_full_success=full)
        except (OSError, ValueError, TypeError, AttributeError):
            pass
        result['service'] = properties(SERVICE)
        if (result['service'].get('ActiveState') == 'failed' and
                result['attempt'].get('outcome') not in ('error', 'warning', 'interrupted', 'deferred')):
            result['attempt'] = dict(outcome='error', message='Verification service failed before a completed result was recorded. Review its journal for details.')
        result['timer'] = properties(TIMER)
        return result
    except (OSError, ValueError, RuntimeError, KeyError, TypeError) as error:
        return dict(mode='manual', scope='latest', error=str(error), attempt={}, last_success={}, last_full_success={})


def hook_script(text):
    text = re.sub(r'^# M45 verification hook\n.*?^# End M45 verification hook\n', '', text, flags=re.M | re.S)
    # Queue backups behind checks instead of the legacy job's silent lock skip.
    if 'flock -n 9 || exit 0' in text:
        text = text.replace('flock -n 9 || exit 0', 'flock 9')
    elif not re.search(r'^flock 9$', text, re.M):
        raise ValueError('Unsupported backup lock; cannot safely serialize verification.')
    anchor = 'm45_finished=1\n' if 'm45_finished=1\n' in text else 'exit "$backup_rc"\n'
    if text.count(anchor) != 1 or 'backup_rc=0' not in text:
        raise ValueError('Unsupported backup completion; no verification hook was installed.')
    hook = ('# M45 verification hook\n'
            'if (( backup_rc <= 1 )); then\n'
            '    /usr/libexec/m45-drive-backuper/backend verification-after-backup || echo "Could not queue backup verification." >&2\n'
            'fi\n# End M45 verification hook\n')
    return text.replace(anchor, hook + anchor)


def ensure_hook(adapter):
    original = adapter.SCRIPT.read_text()
    updated = hook_script(original)
    if updated != original:
        subprocess.run(['/bin/bash', '-n'], input=updated, text=True, capture_output=True, check=True)
        backup = adapter.BACKUPS / ('verification-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
        backup.mkdir(mode=0o700, parents=True)
        adapter.atomic(backup / 'local-system-backup', original, 0o600)
        adapter.atomic(adapter.SCRIPT, updated, 0o755)


def configure(data, adapter):
    config = adapter.read_config()
    if (not isinstance(data, dict) or set(data) != {'mode', 'scope', 'revision'} or
            not isinstance(data['mode'], str) or data['mode'] not in MODES or data['scope'] not in ('latest', 'all') or data['revision'] != revision(config)):
        raise ValueError('Invalid or stale verification settings. Refresh before saving.')
    with adapter.LOCK.open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError('Wait for the backup or verification to finish before saving.') from None
        if data['revision'] != revision(adapter.read_config()):
            raise ValueError('Backup settings changed while saving. Refresh and try again.')
        if adapter.properties(SERVICE).get('ActiveState') in ('active', 'activating', 'deactivating'):
            raise ValueError('Wait for verification to finish before saving.')
        paths = (adapter.SCRIPT, SETTINGS, TIMER_DROPIN)
        saved = {p: (p.read_text(), p.stat().st_mode & 0o777) if p.exists() else None for p in paths}
        was_active = adapter.properties(TIMER).get('ActiveState') == 'active'
        was_enabled = adapter.properties(TIMER).get('UnitFileState') == 'enabled'
        try:
            ensure_hook(adapter)
            adapter.atomic(SETTINGS, json.dumps(dict(mode=data['mode'], scope=data['scope'])) + '\n', 0o644)
            calendar = MODES[data['mode']]
            # Keep a valid timer definition even when disabled in manual or
            # after-backup mode; a timer with no trigger is rejected by systemd.
            adapter.atomic(TIMER_DROPIN, '[Timer]\nOnCalendar=\nOnCalendar=' + (calendar or MODES['weekly']) + '\n', 0o644)
            adapter.run('systemctl', 'daemon-reload')
            adapter.run('systemctl', 'disable', '--now', TIMER)
            if calendar:
                adapter.run('systemctl', 'enable', '--now', TIMER)
        except Exception:
            for path, value in saved.items():
                if value: adapter.atomic(path, *value)
                else: path.unlink(missing_ok=True)
            adapter.run('systemctl', 'daemon-reload', check=False)
            adapter.run('systemctl', 'enable' if was_enabled else 'disable', TIMER, check=False)
            adapter.run('systemctl', 'start' if was_active else 'stop', TIMER, check=False)
            raise
    job_control.discard_idle(adapter,'check')
    return dict(message='Verification settings saved. ' + ('Manual checks only.' if data['mode'] == 'manual' else 'Automatic checking is enabled.'))


def request(adapter, after_backup=False, selection=None):
    pending = None
    if not job_control.allowed('check',manual=not after_backup):
        if after_backup: return dict(message='Verification is paused or postponed.')
        raise ValueError('Verification is paused or postponed. Resume or cancel the postponement first.')
    if after_backup:
        if settings()['mode'] != 'after_backup':
            return dict(message='Automatic after-backup verification is disabled.')
    else:
        with adapter.LOCK.open('a') as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise ValueError('A backup or verification is already running.') from None
            if adapter.properties(SERVICE).get('ActiveState') in ('active', 'activating', 'deactivating'):
                raise ValueError('A verification request is already running or queued.')
            config = adapter.read_config()
            # Older app versions requested a full check without a payload.
            if selection is None:
                selection = dict(scope='all', archive_id='', revision=config['revision'])
            if (not isinstance(selection, dict) or set(selection) != {'scope', 'archive_id', 'revision'} or
                    selection['scope'] not in ('all', 'latest', 'archive') or
                    selection['revision'] != config['revision'] or
                    not isinstance(selection['archive_id'], str) or
                    (selection['scope'] == 'archive' and not re.fullmatch('[a-f0-9]{64}', selection['archive_id'])) or
                    (selection['scope'] != 'archive' and selection['archive_id'])):
                raise ValueError('Invalid or stale check selection. Refresh the backup list and try again.')
            ensure_hook(adapter)
            config = adapter.read_config()
            pending = dict(selection=selection, identity=identity(config), revision=config['revision'],
                           requested_at=time.time(), token=secrets.token_hex(16))
            adapter.atomic(REQUEST, json.dumps(pending)+'\n', 0o600)
    try:
        if not after_backup: job_control.manual_start(adapter,'check')
        adapter.run('systemctl', 'start', '--no-block', SERVICE)
    except Exception as error:
        if pending:
            try:
                if json.loads(REQUEST.read_text()).get('token') == pending['token']: REQUEST.unlink()
            except (OSError, ValueError): pass
        config = adapter.read_config()
        previous, full = history(config)
        attempt = dict(outcome='error', finished_at=datetime.datetime.now(datetime.timezone.utc).isoformat(), message='Could not start verification: ' + str(error))
        adapter.atomic(STATE, json.dumps(dict(identity=identity(config), attempt=attempt, last_success=previous, last_full_success=full))+'\n', 0o644)
        raise
    return dict(message='Backup check requested. Results appear under Saved backups; the check continues with the app closed.')


def check_command(adapter, config, selection, env):
    scope = selection['scope']
    if scope == 'all':
        return ['borg', 'check', '--verify-data', '--info', '--lock-wait', '5', config['repository']], {}
    catalog = json.loads(adapter.run('borg', 'list', '--json', '--glob-archives', 'home-*', '--sort-by', 'timestamp', '--lock-wait', '5',
                                    config['repository'], timeout=90, env=env))['archives']
    complete = [a for a in catalog if not re.search(r'\.checkpoint(?:\.\d+)?$', a['name'])]
    if scope == 'latest':
        if not complete: raise ValueError('There is no completed backup snapshot to check.')
        archive = complete[-1]  # Borg sorts by its actual archive timestamp, including across DST.
    else:
        matches = [a for a in complete if a['id'] == selection['archive_id']]
        if not matches: raise ValueError('The selected completed snapshot is no longer available. Refresh the backup list.')
        archive = matches[0]
    # Borg check --verify-data reads the entire repository even with --last 1.
    # A dry-run extraction validates only this snapshot's metadata and file
    # chunks, without restoring files or claiming the whole repository is sound.
    command = ['borg', 'extract', '--dry-run', '--info', '--lock-wait', '5', config['repository']+'::'+archive['name']]
    return command, dict(archive=archive['name'], archive_id=archive['id'])


def run_check(adapter):
    config = adapter.read_config()
    previous, full = history(config)
    state_identity = identity(config)
    attempt = dict(started_at=datetime.datetime.now(datetime.timezone.utc).isoformat(), outcome='running',
                   message='Preparing backup verification.')

    def save():
        adapter.atomic(STATE, json.dumps(dict(identity=identity(config), attempt=attempt, last_success=previous, last_full_success=full))+'\n', 0o644)

    with adapter.LOCK.open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            attempt.update(outcome='deferred', message='A backup or settings save is running. Verification will retry in five minutes.')
            save()
            return dict(message=attempt['message'], _exit_code=75)
        config = adapter.read_config()
        # Settings can change between the initial read and acquiring the lock.
        if identity(config) != state_identity:
            previous, full = history(config)
        save()
        try:
            selection = dict(scope=settings()['scope'], archive_id='')
            if REQUEST.exists():
                pending = json.loads(REQUEST.read_text())
                REQUEST.unlink()
                if (pending.get('identity') != identity(config) or pending.get('revision') != config['revision'] or
                        not 0 <= time.time() - pending.get('requested_at', 0) <= 3600):
                    raise ValueError('The queued check selection expired or the backup plan changed. Request the check again.')
                selection = pending['selection']
            attempt['scope'] = selection['scope']
            if config.get('auto_mount'):
                adapter.prepare_mounts()
            observed = adapter.run('findmnt', '-rn', '-M', config['mountpoint'], '-o', 'UUID', check=False)
            if observed != config['uuid']:
                raise ValueError('The expected backup filesystem is not mounted.')
            env = dict(adapter.ENV, BORG_CACHE_DIR='/var/cache/local-home-backup',
                       BORG_SECURITY_DIR='/var/lib/local-home-backup/security',
                       BORG_UNKNOWN_UNENCRYPTED_REPO_ACCESS_IS_OK='yes')
            command, target = check_command(adapter, config, selection, env)
            attempt.update(target)
            attempt['message'] = ('Checking the whole repository.' if selection['scope'] == 'all' else 'Reading and validating snapshot ' + target['archive'])
            save()
            # Drain output continuously, retaining a bounded tail for the app.
            # No shell, repairs or lock bypass; snapshot reads always use --dry-run.
            def interrupted(signum, frame):
                raise InterruptedError('Verification stopped before it finished.')
            old_term = signal.signal(signal.SIGTERM, interrupted)
            old_int = signal.signal(signal.SIGINT, interrupted)
            work_dir = tempfile.TemporaryDirectory(prefix='m45-backup-read-')
            process = subprocess.Popen(command,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace', env=env, cwd=work_dir.name)
            tail = ''
            last_update = 0
            for line in process.stdout:
                print(line, end='', flush=True)
                tail = (tail + line)[-32768:]
                if time.monotonic() - last_update >= 3:
                    attempt.update(message=line.strip()[:500], details=tail)
                    save()
                    last_update = time.monotonic()
            rc = process.wait()
            signal.signal(signal.SIGTERM, old_term)
            signal.signal(signal.SIGINT, old_int)
            attempt.update(exit_code=rc, details=tail, outcome='success' if rc == 0 else ('warning' if rc == 1 else 'error'),
                           message=('Full Borg repository integrity check passed.' if selection['scope'] == 'all' else 'Snapshot read verification passed: ' + target['archive']) if rc == 0 else 'Borg reported problems. Review verification details before relying on this backup.')
        except InterruptedError as error:
            if 'process' in locals() and process.poll() is None:
                process.terminate()
                try: process.wait(timeout=10)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
            attempt.update(outcome='interrupted', message=str(error), exit_code=130)
        except Exception as error:
            attempt.update(outcome='error', message=str(error), exit_code=2)
        finally:
            if 'process' in locals():
                if process.poll() is None:
                    process.terminate()
                    try: process.wait(timeout=10)
                    except subprocess.TimeoutExpired: process.kill(); process.wait()
                if process.stdout is not None: process.stdout.close()
            if 'work_dir' in locals(): work_dir.cleanup()
            if 'old_term' in locals():
                signal.signal(signal.SIGTERM, old_term)
                signal.signal(signal.SIGINT, old_int)
        if 'tail' in locals(): attempt['details'] = tail
        attempt['finished_at'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        if attempt['outcome'] == 'success':
            previous = dict(attempt)
            if attempt['scope'] == 'all': full = dict(attempt)
        save()
    return dict(message=attempt['message'], _exit_code=attempt.get('exit_code', 2))
