#!/usr/bin/python3
"""Fixed-purpose adapter for this machine's existing Borg/systemd job."""
import base64
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

SCRIPT = Path('/usr/local/sbin/local-system-backup')
EXCLUDES = Path('/etc/local-system-backup.exclude')
INTERVAL = Path('/etc/systemd/system/local-system-backup.timer.d/interval.conf')
SERVICE = 'local-system-backup.service'
TIMER = 'local-system-backup.timer'
LOCK = Path('/run/lock/local-system-backup.lock')
BACKUPS = Path('/var/lib/m45-drive-backuper/settings-backups')
ENV = dict(os.environ, PATH='/usr/sbin:/usr/bin:/sbin:/bin', LC_ALL='C')


def run(*args, timeout=15, env=None, check=True):
    result = subprocess.run(args, text=True, capture_output=True, timeout=timeout, env=env or ENV)
    if check and result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip() or f'{args[0]} exited {result.returncode}')
    return result.stdout.strip()


def properties(unit):
    return dict(line.split('=', 1) for line in run(
        'systemctl', 'show', unit, '--no-pager', '-p', 'ActiveState', '-p', 'SubState',
        '-p', 'Result', '-p', 'ExecMainStartTimestamp', '-p', 'ExecMainExitTimestamp',
        '-p', 'NextElapseUSecRealtime', '-p', 'UnitFileState', '-p', 'TimersCalendar').splitlines() if '=' in line)


def read_config():
    text = SCRIPT.read_text()
    uuid = re.search(r'^uuid=([a-f0-9-]+)$', text, re.M)
    mount = re.search(r'^mountpoint=(.+)$', text, re.M)
    retention = re.search(r'--keep-hourly (\d+) --keep-daily (\d+) --keep-weekly (\d+) --keep-monthly (\d+)', text)
    if not uuid or not mount or not retention:
        raise ValueError('Backup script format is unsupported; no changes were made.')
    mountpoint = shlex.split(mount[1])[0].replace('$uuid', uuid[1])
    interval = INTERVAL.read_text() if INTERVAL.exists() else ''
    match = re.search(r'OnCalendar=\*-\*-\* 00/(\d+):00:00', interval)
    hourly = not interval or 'OnCalendar=hourly' in interval
    daily = 'OnCalendar=daily' in interval
    excludes = EXCLUDES.read_text()
    revision = hashlib.sha256(json.dumps([text, excludes, interval]).encode()).hexdigest()
    return dict(uuid=uuid[1], mountpoint=mountpoint, hours=int(match[1]) if match else (24 if daily else (1 if hourly else 0)), revision=revision,
                hourly=int(retention[1]), daily=int(retention[2]), weekly=int(retention[3]), monthly=int(retention[4]),
                excludes=excludes, source='/home/dist', compression='zstd,3')


def actual_mount(uuid):
    return run('findmnt', '-rn', '-S', 'UUID=' + uuid, '-o', 'TARGET', check=False).splitlines()


def status():
    config = read_config()
    service, timer = properties(SERVICE), properties(TIMER)
    observed = run('findmnt', '-rn', '-M', config['mountpoint'], '-o', 'UUID', check=False)
    mounted = observed == config['uuid']
    result = dict(collected_at=int(time.time()), config=config, service=service, timer=timer,
                  mounted=mounted, actual_mounts=actual_mount(config['uuid']))
    if mounted:
        usage = shutil.disk_usage(config['mountpoint'])
        result.update(free=usage.free, total=usage.total)
    logs = subprocess.run(['journalctl', '-u', SERVICE, '-n', '250', '--no-pager', '-o', 'short-iso'],
                          text=True, capture_output=True, timeout=15, env=ENV)
    result['logs'] = logs.stdout.strip()
    result['log_error'] = logs.stderr.strip() if logs.returncode or not logs.stdout.strip() else ''
    return result


def validate(data):
    expected = {'mountpoint', 'hours', 'hourly', 'daily', 'weekly', 'monthly', 'excludes', 'revision'}
    if not isinstance(data, dict) or set(data) != expected:
        raise ValueError('Unexpected configuration fields.')
    if not isinstance(data['revision'], str) or not re.fullmatch('[a-f0-9]{64}', data['revision']):
        raise ValueError('Invalid configuration revision; reopen settings.')
    for key in ('hours', 'hourly', 'daily', 'weekly', 'monthly'):
        if type(data[key]) is not int:
            raise ValueError('Retention and schedule must be integers.')
    if data['hours'] not in (1, 2, 3, 4, 6, 8, 12, 24):
        raise ValueError('Unsupported schedule.')
    if any(not 0 <= data[key] <= 365 for key in ('hourly', 'daily', 'weekly', 'monthly')) or not any(data[key] for key in ('hourly', 'daily', 'weekly', 'monthly')):
        raise ValueError('Keep at least one archive; retention must be between 0 and 365.')
    mount = data['mountpoint']
    if not isinstance(mount, str) or not re.fullmatch(r'/[A-Za-z0-9_./ -]{1,240}', mount) or '..' in Path(mount).parts:
        raise ValueError('Use an absolute mount path with letters, numbers, spaces, dots, dashes or underscores.')
    if not isinstance(data['excludes'], str) or len(data['excludes'].encode()) > 32768 or '\0' in data['excludes']:
        raise ValueError('Exclusions must be text smaller than 32 KiB.')
    # Validate using Borg's own parser, including supported pattern prefixes.
    from borg.patterns import parse_pattern
    for line in data['excludes'].splitlines():
        if line.strip() and not line.lstrip().startswith('#'):
            parse_pattern(line.strip())
    return data


def replacement(text, data):
    text, count = re.subn(r'^mountpoint=.+$', 'mountpoint=' + shlex.quote(data['mountpoint']), text, flags=re.M)
    if count != 1:
        raise ValueError('Unsupported mountpoint declaration.')
    retention = ' '.join(f'--keep-{key} {data[key]}' for key in ('hourly', 'daily', 'weekly', 'monthly'))
    text, count = re.subn(r'--keep-hourly \d+ --keep-daily \d+ --keep-weekly \d+ --keep-monthly \d+', retention, text)
    if count != 1:
        raise ValueError('Unsupported retention declaration.')
    return text


def atomic(path, content, mode):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix='.m45-drive-backuper-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as stream:
            os.fchmod(stream.fileno(), mode)
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def apply(data):
    validate(data)
    old = read_config()
    if old['revision'] != data['revision']:
        raise ValueError('Backup settings changed outside this editor. Reopen M45 Drive Backuper before saving.')
    if data['mountpoint'] != old['mountpoint']:
        observed = run('findmnt', '-rn', '-M', data['mountpoint'], '-o', 'UUID', check=False)
        if observed != old['uuid']:
            raise ValueError('The new mount path must contain the existing UUID-verified backup filesystem.')
    if properties(SERVICE)['ActiveState'] in ('activating', 'active', 'deactivating'):
        raise ValueError('Wait for the running backup to finish before changing settings.')
    with LOCK.open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError('A backup is running; try again after it finishes.') from None
        original = SCRIPT.read_text()
        updated = replacement(original, data)
        subprocess.run(['/bin/bash', '-n'], input=updated, text=True, check=True, capture_output=True, env=ENV)
        targets = {SCRIPT: (updated, 0o755), EXCLUDES: (data['excludes'], 0o644), INTERVAL: (
            '[Unit]\nDescription=Home backup schedule managed by M45 Drive Backuper\n\n[Timer]\nOnCalendar=\n' +
            ('OnCalendar=daily\n' if data['hours'] == 24 else f"OnCalendar=*-*-* 00/{data['hours']}:00:00\n"), 0o644)}
        saved = {p: (p.read_text(), p.stat().st_mode & 0o777) if p.exists() else None for p in targets}
        # Root-only recovery copies; never copy user-supplied paths.
        backup = BACKUPS / datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
        backup.mkdir(mode=0o700, parents=True)
        for p, value in saved.items():
            if value:
                atomic(backup / p.name, value[0], 0o600)
        try:
            for p, (content, mode) in targets.items():
                atomic(p, content, mode)
            run('systemctl', 'daemon-reload')
            if properties(TIMER)['ActiveState'] == 'active':
                run('systemctl', 'restart', TIMER)
        except Exception:
            for p, value in saved.items():
                if value:
                    atomic(p, value[0], value[1])
                else:
                    p.unlink(missing_ok=True)
            run('systemctl', 'daemon-reload', check=False)
            raise
    return dict(message='Backup settings saved. Recovery copies: ' + str(backup))


def archives():
    config = read_config()
    if run('findmnt', '-rn', '-M', config['mountpoint'], '-o', 'UUID', check=False) != config['uuid']:
        raise ValueError('The configured backup filesystem is not mounted.')
    repo = Path(config['mountpoint']) / ('borg-home-' + Path('/etc/machine-id').read_text().strip())
    env = dict(ENV, BORG_CACHE_DIR='/var/cache/local-home-backup', BORG_SECURITY_DIR='/var/lib/local-home-backup/security',
               BORG_UNKNOWN_UNENCRYPTED_REPO_ACCESS_IS_OK='yes')
    return json.loads(run('borg', 'list', '--json', '--lock-wait', '1', '--glob-archives', 'home-*', str(repo), timeout=90, env=env))


def main():
    args = sys.argv[1:]
    if args == ['status']:
        return status()
    if not args or args[0] not in ('apply', 'start', 'enable', 'disable', 'archives'):
        raise ValueError('Unsupported action.')
    if os.geteuid() != 0:
        raise PermissionError('This action requires administrator authentication.')
    if args[0] == 'apply' and len(args) == 2 and len(args[1]) <= 60000:
        return apply(json.loads(base64.b64decode(args[1], validate=True)))
    if len(args) != 1:
        raise ValueError('Unexpected arguments.')
    if args[0] == 'archives':
        return archives()
    command = {'start': ('start', '--no-block', SERVICE), 'enable': ('enable', '--now', TIMER),
               'disable': ('disable', '--now', TIMER)}[args[0]]
    run('systemctl', *command)
    return dict(message={'start': 'Backup requested; check the service status and journal.',
                         'enable': 'Automatic backups enabled.', 'disable': 'Automatic backups disabled. An active backup will finish.'}[args[0]])


if __name__ == '__main__':
    try:
        print(json.dumps(main()))
    except Exception as error:
        print(json.dumps({'error': str(error)}))
        sys.exit(1)
