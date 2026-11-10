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
import recovery
import reporting

SCRIPT = Path('/usr/local/sbin/local-system-backup')
EXCLUDES = Path('/etc/local-system-backup.exclude')
INTERVAL = Path('/etc/systemd/system/local-system-backup.timer.d/interval.conf')
SERVICE = 'local-system-backup.service'
TIMER = 'local-system-backup.timer'
LOCK = Path('/run/lock/local-system-backup.lock')
BACKUPS = Path('/var/lib/m45-drive-backuper/settings-backups')
RECOVERY = Path('/var/lib/m45-drive-backuper/recovery/software-recovery.json')
ENV = dict(os.environ, PATH='/usr/sbin:/usr/bin:/sbin:/bin', LC_ALL='C')
CONFIG_FIELDS = {'mountpoint', 'hours', 'hourly', 'daily', 'weekly', 'monthly', 'excludes', 'revision',
                 'source', 'source_kind', 'compression', 'scan_mode', 'reserve_gib', 'quota_gib',
                 'exclude_caches', 'auto_mount', 'repository', 'destination_kind', 'include_system', 'package_recovery'}


def run(*args, timeout=15, env=None, check=True):
    result = subprocess.run(args, text=True, capture_output=True, timeout=timeout, env=env or ENV)
    if check and result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip() or f'{args[0]} exited {result.returncode}')
    return result.stdout.strip()


def properties(unit):
    return dict(line.split('=', 1) for line in run(
        'systemctl', 'show', unit, '--no-pager', '-p', 'ActiveState', '-p', 'SubState',
        '-p', 'Result', '-p', 'ExecMainStartTimestamp', '-p', 'ExecMainExitTimestamp',
        '-p', 'NextElapseUSecRealtime', '-p', 'UnitFileState', '-p', 'TimersCalendar',
        '-p', 'InvocationID', '-p', 'ExecMainStatus', '-p', 'ExecMainCode').splitlines() if '=' in line)


def read_config():
    text = SCRIPT.read_text()
    uuid = re.search(r'^uuid=([a-fA-F0-9-]+)$', text, re.M)
    mount = re.search(r'^mountpoint=(.+)$', text, re.M)
    retention = re.search(r'--keep-hourly (\d+) --keep-daily (\d+) --keep-weekly (\d+) --keep-monthly (\d+)', text)
    if not uuid or not mount or not retention:
        raise ValueError('Backup script format is unsupported; no changes were made.')
    mountpoint = shlex.split(mount[1])[0].replace('$uuid', uuid[1])
    repository = re.search(r'^repository=(.+)$', text, re.M)
    repository = shlex.split(repository[1])[0] if repository else str(Path(mountpoint) / ('borg-home-' + Path('/etc/machine-id').read_text().strip()))
    destination_kind = re.search(r'^destination_kind=(folder|drive)$', text, re.M)
    interval = INTERVAL.read_text() if INTERVAL.exists() else ''
    match = re.search(r'OnCalendar=\*-\*-\* 00/(\d+):00:00', interval)
    hourly = not interval or 'OnCalendar=hourly' in interval
    daily = 'OnCalendar=daily' in interval
    excludes = EXCLUDES.read_text()
    revision = hashlib.sha256(json.dumps([text, excludes, interval]).encode()).hexdigest()
    source = re.search(r'^source=(.+)$', text, re.M)
    source_kind = re.search(r'^source_kind=(folder|drive)$', text, re.M)
    source_uuid = re.search(r'^source_uuid=(.+)$', text, re.M)
    source_mount = re.search(r'^source_mountpoint=(.+)$', text, re.M)
    compression = re.search(r'--compression (\S+)', text)
    reserve = re.search(r'\(\( available > (\d+) \)\)', text)
    quota = re.search(r'^# M45 storage limit GiB: (\d+)$', text, re.M)
    return dict(uuid=uuid[1], mountpoint=mountpoint, repository=repository, destination_kind=destination_kind[1] if destination_kind else 'folder', hours=int(match[1]) if match else (24 if daily else (1 if hourly else 0)), revision=revision,
                hourly=int(retention[1]), daily=int(retention[2]), weekly=int(retention[3]), monthly=int(retention[4]),
                excludes=excludes, source=shlex.split(source[1])[0] if source else '/home/dist',
                source_kind=source_kind[1] if source_kind else 'folder',
                source_uuid=shlex.split(source_uuid[1])[0] if source_uuid else '',
                source_mountpoint=shlex.split(source_mount[1])[0] if source_mount else '',
                auto_mount='# M45 auto mount: yes' in text,
                include_system='# M45 include system settings: yes' in text,
                package_recovery='# M45 software recovery: yes' in text,
                warning_reporting='# M45 Borg reporting' in text,
                compression=compression[1] if compression else 'zstd,3',
                scan_mode='full_scan' if '--files-cache disabled' in text else 'incremental',
                reserve_gib=int(reserve[1]) // 1073741824 if reserve else 10,
                quota_gib=int(quota[1]) if quota else -1, exclude_caches='--exclude-caches' in text)


def actual_mount(uuid):
    output = run('findmnt', '--json', '-S', 'UUID=' + uuid, '-o', 'TARGET', check=False)
    return [fs['target'] for fs in json.loads(output or '{}').get('filesystems', [])]


def mounted_at(target):
    output = run('findmnt', '--json', '-M', target, '-o', 'TARGET,UUID', check=False)
    entries = json.loads(output or '{}').get('filesystems', [])
    return entries[0] if entries else None


def ensure_mount(target, uuid):
    """Mount only an existing fstab entry with the expected UUID and target."""
    observed = mounted_at(target)
    if observed:
        if observed.get('uuid') != uuid:
            raise ValueError('A different filesystem is mounted at ' + target)
        return
    entries = json.loads(run('findmnt', '--fstab', '--evaluate', '--json', '-o', 'TARGET,UUID', check=False) or '{}').get('filesystems', [])
    if not any(entry.get('uuid') == uuid and os.path.normpath(entry.get('target', '')) == os.path.normpath(target) for entry in entries):
        raise ValueError('Automatic mounting needs an existing /etc/fstab entry for UUID ' + uuid + ' at ' + target)
    run('mount', '--', target, timeout=45)
    observed = mounted_at(target)
    if not observed or observed.get('uuid') != uuid:
        raise ValueError('The expected filesystem could not be mounted at ' + target)


def prepare_mounts():
    config = read_config()
    if not config['auto_mount']:
        raise ValueError('Enable “Mount configured drives when needed” in the backup plan first.')
    if config.get('source_uuid'):
        ensure_mount(config.get('source_mountpoint') or config['source'], config['source_uuid'])
    ensure_mount(config['mountpoint'], config['uuid'])
    return dict(message='Configured backup drives are mounted and their UUIDs are verified.')


def retain_last_attempt(snapshot):
    """Keep observed completed attempts across systemd's reboot-time reset."""
    config, service = snapshot['config'], snapshot['service']
    identity = {key: config.get(key, '') for key in ('uuid', 'repository', 'source')}
    path = Path(os.environ.get('XDG_STATE_HOME', str(Path.home() / '.local/state'))) / 'm45-drive-backuper/last-attempt.json'
    # A new invocation, including one without a result yet, must never inherit
    # diagnostics from the previous attempt.
    if service.get('ActiveState') in ('active', 'activating', 'deactivating'):
        return
    fields = ('service', 'borg_report', 'logs', 'activity', 'log_truncated')
    try:
        if service.get('ExecMainExitTimestamp') and service.get('Result'):
            saved = dict(version=1, identity=identity,
                         attempt={key: snapshot[key] for key in fields})
            path.parent.mkdir(parents=True, exist_ok=True)
            temporary = None
            try:
                with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, delete=False) as output:
                    temporary = Path(output.name)
                    json.dump(saved, output)
                temporary.replace(path)
            finally:
                if temporary is not None:
                    temporary.unlink(missing_ok=True)
        elif not service.get('ExecMainStartTimestamp') and not service.get('InvocationID') and service.get('ActiveState') != 'failed':
            saved = json.loads(path.read_text())
            if saved.get('version') == 1 and saved.get('identity') == identity:
                attempt = saved['attempt']
                if (all(key in attempt for key in fields)
                        and attempt['service'].get('ExecMainExitTimestamp')
                        and attempt['service'].get('Result')):
                    snapshot.update(attempt)
                    snapshot['previous_attempt'] = True
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        # Missing history or an unwritable state directory must not break live
        # monitoring or turn an unknown result into a successful one.
        pass


def status():
    config = read_config()
    service, timer = properties(SERVICE), properties(TIMER)
    observed = run('findmnt', '-rn', '-M', config['mountpoint'], '-o', 'UUID', check=False)
    mounted = observed == config['uuid']
    result = dict(collected_at=int(time.time()), config=config, service=service, timer=timer,
                  mounted=mounted, actual_mounts=actual_mount(config['uuid']))
    result['source_ready'] = Path(config['source']).is_dir()
    if config.get('source_uuid'):
        source_mount = mounted_at(config.get('source_mountpoint') or config['source'])
        result['source_ready'] = result['source_ready'] and bool(source_mount and source_mount.get('uuid') == config['source_uuid'])
    if mounted:
        usage = shutil.disk_usage(config['mountpoint'])
        result.update(free=usage.free, total=usage.total)
    invocation = service.get('InvocationID', '')
    journal_args = ['journalctl', '-u', SERVICE, '-n', '500', '--no-pager', '-o', 'json']
    if re.fullmatch('[a-f0-9]{32}', invocation):
        journal_args.append('_SYSTEMD_INVOCATION_ID=' + invocation)
    logs = subprocess.run(journal_args,
                          text=True, capture_output=True, timeout=15, env=ENV)
    report, readable, activity = reporting.parse_journal(logs.stdout.strip())
    # Never use an older invocation's diagnostics as the current attempt result.
    result['borg_report'] = report if re.fullmatch('[a-f0-9]{32}', invocation) else dict(outcome='unknown', issues=[])
    result['logs'] = readable
    result['activity'] = activity
    result['log_truncated'] = len(logs.stdout.splitlines()) >= 500
    result['log_error'] = logs.stderr.strip() if logs.returncode or not logs.stdout.strip() else ''
    retain_last_attempt(result)
    return result


def validate(data):
    if not isinstance(data, dict) or set(data) != CONFIG_FIELDS:
        raise ValueError('Unexpected configuration fields.')
    if not isinstance(data['revision'], str) or not re.fullmatch('[a-f0-9]{64}', data['revision']):
        raise ValueError('Invalid configuration revision; reopen settings.')
    if data['source_kind'] not in ('folder', 'drive') or data['destination_kind'] not in ('folder', 'drive') or data['scan_mode'] not in ('incremental', 'full_scan'):
        raise ValueError('Unsupported backup type.')
    if data['compression'] not in ('none', 'lz4', 'zstd,3', 'zstd,6', 'zstd,9'):
        raise ValueError('Unsupported compression option.')
    for key, minimum in (('reserve_gib', 1), ('quota_gib', -1)):
        if type(data[key]) is not int or not minimum <= data[key] <= 1048576:
            raise ValueError('Storage limits must be whole GiB values within range.')
    if type(data['exclude_caches']) is not bool or type(data['auto_mount']) is not bool:
        raise ValueError('Cache and automatic mount options must be checkbox values.')
    if any(type(data[key]) is not bool for key in ('include_system', 'package_recovery')):
        raise ValueError('System settings and software recovery must be checkbox values.')
    source = data['source']
    if not isinstance(source, str) or not source.startswith('/') or len(source) > 4096 or any(c in source for c in '\0\n\r') or '..' in Path(source).parts:
        raise ValueError('Choose an absolute source folder or mounted drive.')
    repository = data['repository']
    if not isinstance(repository, str) or (repository and (not repository.startswith('/') or len(repository) > 4096 or any(c in repository for c in '\0\n\r') or '..' in Path(repository).parts)):
        raise ValueError('Choose an absolute backup folder.')
    for key in ('hours', 'hourly', 'daily', 'weekly', 'monthly'):
        if type(data[key]) is not int:
            raise ValueError('Retention and schedule must be integers.')
    if data['hours'] not in (1, 2, 3, 4, 6, 8, 12, 24):
        raise ValueError('Unsupported schedule.')
    if any(not 0 <= data[key] <= 365 for key in ('hourly', 'daily', 'weekly', 'monthly')) or not any(data[key] for key in ('hourly', 'daily', 'weekly', 'monthly')):
        raise ValueError('Keep at least one archive; retention must be between 0 and 365.')
    mount = data['mountpoint']
    if not isinstance(mount, str) or not mount.startswith('/') or len(mount) > 4096 or any(c in mount for c in '\0\n\r') or '..' in Path(mount).parts:
        raise ValueError('Use an absolute mount path without line breaks.')
    if not isinstance(data['excludes'], str) or len(data['excludes'].encode()) > 32768 or '\0' in data['excludes']:
        raise ValueError('Exclusions must be text smaller than 32 KiB.')
    # Validate using Borg's own parser, including supported pattern prefixes.
    from borg.patterns import parse_pattern
    for line in data['excludes'].splitlines():
        if line.strip() and not line.lstrip().startswith('#'):
            parse_pattern(line.strip())
    return data


def replacement(text, data):
    text = re.sub(r'^destination_kind=.+\n', '', text, flags=re.M)
    text, count = re.subn(r'^mountpoint=.+$', lambda _: 'mountpoint=' + shlex.quote(data['mountpoint']), text, flags=re.M)
    if count != 1:
        raise ValueError('Unsupported mountpoint declaration.')
    text = re.sub(r'^mountpoint=.+$', lambda m: m[0] + '\ndestination_kind=' + data['destination_kind'], text, flags=re.M)
    if data.get('destination_uuid'):
        text = re.sub(r'^uuid=.+$', lambda _: 'uuid=' + shlex.quote(data['destination_uuid']), text, flags=re.M)
    if data.get('repository'):
        text = re.sub(r'^repository=.+\n', '', text, flags=re.M)
        text, count = re.subn(r'^export BORG_REPO=.+$', lambda _: 'repository=' + shlex.quote(data['repository']) + '\nexport BORG_REPO="$repository"', text, flags=re.M)
        if count != 1:
            raise ValueError('Unsupported repository declaration.')
        text = text.replace(' --exclude "pp:${BORG_REPO#/}"','')
        text = text.replace('borg create --stats --show-rc','borg create --stats --show-rc --exclude "pp:${BORG_REPO#/}"')
    retention = ' '.join(f'--keep-{key} {data[key]}' for key in ('hourly', 'daily', 'weekly', 'monthly'))
    text, count = re.subn(r'--keep-hourly \d+ --keep-daily \d+ --keep-weekly \d+ --keep-monthly \d+', retention, text)
    if count != 1:
        raise ValueError('Unsupported retention declaration.')
    # Preserve the existing job's locking, UUID checks, repository and cleanup flow.
    text = re.sub(r'^source(?:_kind|_uuid|_mountpoint)?=.+\n', '', text, flags=re.M)
    text = re.sub(r'^# M45 source checks\n.*?^# End M45 source checks\n', '', text, flags=re.M | re.S)
    checks = '# M45 source checks\n'
    if data['source_kind'] == 'drive' or data['auto_mount']:
        if not data.get('source_uuid'):
            raise ValueError('The source drive needs a filesystem UUID.')
        checks += 'source_uuid=' + shlex.quote(data['source_uuid']) + '\nsource_mountpoint=' + shlex.quote(data.get('source_mountpoint') or data['source']) + '\n'
    if data['auto_mount']:
        checks += '# M45 auto mount: yes\n/usr/libexec/m45-drive-backuper/backend mount\n'
    if data.get('source_uuid'):
        checks += '[[ $(findmnt -rn -M "$source_mountpoint" -o UUID) == "$source_uuid" ]] || { echo "Expected source drive is not mounted." >&2; exit 1; }\n'
    checks += '[[ -d "$source" ]] || { echo "Backup source is unavailable." >&2; exit 1; }\n'
    checks += 'exec 8<"$source"\nbackup_sources=("$source")\n'
    if data['include_system']:
        checks += '# M45 include system settings: yes\n'
        checks += 'for extra in ' + ' '.join(map(shlex.quote,recovery.SYSTEM_PATHS)) + '; do\n'
        checks += '    [[ -e "$extra" ]] || continue\n    if [[ "$source" != / && "$extra" != "$source" && "$extra" != "$source/"* ]]; then backup_sources+=("$extra"); fi\ndone\n'
    checks += '# End M45 source checks\n'
    text = re.sub(r'^mountpoint=.+$', lambda m: m[0] + '\nsource=' + shlex.quote(data['source']) + '\nsource_kind=' + data['source_kind'] + '\n' + checks.rstrip(), text, flags=re.M)
    text, count = re.subn(r"(^\s*'::home-\{now:[^']+\}' )(.+?)( \|\| backup_rc=\$\?)$", lambda m: m[1] + '"${backup_sources[@]}"' + m[3], text, flags=re.M)
    if count != 1:
        raise ValueError('Unsupported backup source command.')
    compression = '--compression ' + data['compression'] + (' --files-cache disabled' if data['scan_mode'] == 'full_scan' else '')
    text, count = re.subn(r'--compression \S+(?: --files-cache \S+)?', compression, text)
    if count != 1:
        raise ValueError('Unsupported compression command.')
    text = text.replace(' --exclude-caches', '')
    if data['exclude_caches']:
        text = text.replace('--exclude-from /etc/local-system-backup.exclude', '--exclude-from /etc/local-system-backup.exclude --exclude-caches')
    text, count = re.subn(r"^\(\( available > \d+ \)\).+$", f"(( available > {data['reserve_gib'] * 1073741824} )) || {{ echo 'Less than {data['reserve_gib']} GiB free after cleanup; backup stopped.' >&2; exit 1; }}", text, flags=re.M)
    if count != 1:
        raise ValueError('Unsupported free-space guard.')
    text = re.sub(r'^# M45 software recovery: yes\n.*?^# End M45 software recovery\n', '', text, flags=re.M | re.S)
    if data['package_recovery']:
        block = '# M45 software recovery: yes\n/usr/libexec/m45-drive-backuper/backend recovery-refresh\nbackup_sources+=(/var/lib/m45-drive-backuper/recovery)\n# End M45 software recovery\n'
        text, count = re.subn(r'^(# Reclaim abandoned transaction data.*)$', lambda m: block + m[0], text, flags=re.M)
        if count != 1:
            raise ValueError('Unsupported repository setup block.')
    if data['quota_gib'] >= 0:
        text = re.sub(r'^# M45 storage limit GiB: \d+\n.*?^# End M45 storage limit\n', '', text, flags=re.M | re.S)
        quota = f"borg config \"$BORG_REPO\" storage_quota {data['quota_gib'] * 1073741824}\n" if data['quota_gib'] else 'if borg config "$BORG_REPO" storage_quota >/dev/null 2>&1; then borg config --delete "$BORG_REPO" storage_quota; fi\n'
        block = f"# M45 storage limit GiB: {data['quota_gib']}\n" + quota + '# End M45 storage limit\n'
        text, count = re.subn(r'^(# Reclaim abandoned transaction data.*)$', lambda m: block + m[0], text, flags=re.M)
        if count != 1:
            raise ValueError('Unsupported repository setup block.')
    return reporting.upgrade_script(text)


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


def configure_destination(data, old, source):
    """Use an empty folder or this machine's existing managed repository."""
    path = Path(data['repository'])
    resolved = path.resolve()
    if str(resolved) != str(path) or resolved == source or source.is_relative_to(resolved):
        raise ValueError('Choose a resolved backup folder that does not contain the source.')
    parent = path
    while not parent.exists():
        parent = parent.parent
    filesystem = json.loads(run('findmnt','--json','-T',str(parent),'-o','TARGET,UUID')).get('filesystems',[{}])[0]
    uuid, mountpoint = filesystem.get('uuid'), filesystem.get('target')
    if not uuid or not re.fullmatch('[a-fA-F0-9-]+',uuid) or not mountpoint:
        raise ValueError('Choose a folder on a connected local drive with a filesystem UUID.')
    if data['destination_kind'] == 'drive' and Path(mountpoint) != Path(data['mountpoint']):
        raise ValueError('The selected backup drive is no longer available. Reconnect and mount it before saving.')
    if (old.get('mountpoint') and path.is_relative_to(Path(old['mountpoint']))
            and Path(old['mountpoint']).is_relative_to(Path(mountpoint)) and uuid != old.get('uuid')):
        raise ValueError('The expected backup drive is unavailable at this folder. Reconnect it or choose another destination.')
    if path == Path(mountpoint):
        raise ValueError('Choose a dedicated backup folder inside the drive, or select “This drive” in the app.')
    marker = path/'.m45-drive-backuper'
    machine = Path('/etc/machine-id').read_text().strip()
    env = dict(ENV,BORG_CACHE_DIR='/var/cache/local-home-backup',BORG_SECURITY_DIR='/var/lib/local-home-backup/security',BORG_UNKNOWN_UNENCRYPTED_REPO_ACCESS_IS_OK='yes',BORG_RELOCATED_REPO_ACCESS_IS_OK='yes',BORG_PASSPHRASE='')
    if path.exists() and (not path.is_dir() or any(path.iterdir())):
        managed = marker.is_file() and marker.read_text().strip() == machine
        legacy = path.name == 'borg-home-' + machine
        borg_layout = (path/'config').is_file() and (path/'data').is_dir()
        if not managed and not legacy and not borg_layout:
            raise ValueError('This folder contains other files. Create an empty backup folder here instead.')
        run('borg','info','--json','--lock-wait','1',str(path),env=env,timeout=90)
    else:
        path.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
        run('borg','init','--encryption=none',str(path),env=env,timeout=90)
        atomic(marker,machine+'\n',0o600)
    data.update(repository=str(path),mountpoint=mountpoint,destination_uuid=uuid)


def apply(data):
    validate(data)
    old = read_config()
    if old['revision'] != data['revision']:
        raise ValueError('Backup settings changed outside this editor. Reopen M45 Drive Backuper before saving.')
    source = Path(data['source']).resolve(strict=True)
    if str(source) != str(Path(data['source'])):
        raise ValueError('Use the resolved source path so exclusions match: ' + str(source))
    destination = Path(data['mountpoint']).resolve()
    repository = Path(data['repository']).resolve() if data['repository'] else destination
    if not source.is_dir() or source.is_relative_to(repository):
        raise ValueError('Choose a source folder outside the backup destination.')
    if data['include_system'] and any(Path(p).is_relative_to(repository) for p in recovery.SYSTEM_PATHS):
        raise ValueError('The backup destination cannot contain the system settings being backed up.')
    data = dict(data, source=str(source))
    if data['source_kind'] == 'drive':
        data['source_uuid'] = run('findmnt', '-rn', '-M', str(source), '-o', 'UUID', check=False)
        if not re.fullmatch(r'[A-Za-z0-9-]+', data['source_uuid']):
            raise ValueError('The selected source drive must be mounted and have a filesystem UUID.')
    if data['auto_mount']:
        output = run('findmnt', '--json', '-T', str(source), '-o', 'TARGET,UUID')
        filesystem = json.loads(output).get('filesystems', [{}])[0]
        if not filesystem.get('uuid') or not filesystem.get('target'):
            raise ValueError('Automatic mounting needs a source filesystem with a UUID. Connect it while saving this option.')
        data.update(source_uuid=filesystem['uuid'], source_mountpoint=filesystem['target'])
    changing_repository = bool(data['repository'] and data['repository'] != old.get('repository'))
    if not changing_repository and data['mountpoint'] != old['mountpoint']:
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
        # Reject unsupported scripts before creating even an empty repository.
        preview = replacement(original, data)
        subprocess.run(['/bin/bash', '-n'], input=preview, text=True, check=True, capture_output=True, env=ENV)
        if changing_repository:
            configure_destination(data,old,source)
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
    repo = Path(config['repository'])
    env = dict(ENV, BORG_CACHE_DIR='/var/cache/local-home-backup', BORG_SECURITY_DIR='/var/lib/local-home-backup/security',
               BORG_UNKNOWN_UNENCRYPTED_REPO_ACCESS_IS_OK='yes')
    return json.loads(run('borg', 'list', '--json', '--lock-wait', '1', '--glob-archives', 'home-*', str(repo), timeout=90, env=env))


def main():
    args = sys.argv[1:]
    if args == ['status']:
        return status()
    if args in (['recovery-export'], ['packages-preview']):
        if args[0] == 'recovery-export':
            return dict(manifest=recovery.inventory(read_config()['source']))
        return recovery.restore(read_manifest())
    if not args or args[0] not in ('apply', 'start', 'enable', 'disable', 'archives', 'mount', 'recovery-refresh', 'packages-install', 'upgrade-reporting'):
        raise ValueError('Unsupported action.')
    if os.geteuid() != 0:
        raise PermissionError('This action requires administrator authentication.')
    if args[0] == 'apply' and len(args) == 2 and len(args[1]) <= 60000:
        return apply(json.loads(base64.b64decode(args[1], validate=True)))
    if len(args) != 1:
        raise ValueError('Unexpected arguments.')
    if args[0] == 'upgrade-reporting':
        config = read_config()
        return apply({key: config[key] for key in CONFIG_FIELDS})
    if args[0] == 'recovery-refresh':
        RECOVERY.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        manifest = recovery.inventory(read_config()['source'])
        atomic(RECOVERY,json.dumps(manifest,indent=2)+'\n',0o600)
        return dict(message='Software recovery list refreshed.', warnings=manifest['warnings'])
    if args[0] == 'packages-install':
        return recovery.restore(read_manifest(),apply=True)
    if args[0] == 'archives':
        return archives()
    if args[0] == 'mount':
        return prepare_mounts()
    command = {'start': ('start', '--no-block', SERVICE), 'enable': ('enable', '--now', TIMER),
               'disable': ('disable', '--now', TIMER)}[args[0]]
    run('systemctl', *command)
    return dict(message={'start': 'Backup requested; check the service status and journal.',
                         'enable': 'Automatic backups enabled.', 'disable': 'Automatic backups disabled. An active backup will finish.'}[args[0]])


def read_manifest():
    content = sys.stdin.read(4 * 1024 * 1024 + 1)
    if len(content) > 4 * 1024 * 1024:
        raise ValueError('Recovery files must be smaller than 4 MiB.')
    return json.loads(content)


if __name__ == '__main__':
    try:
        print(json.dumps(main()))
    except Exception as error:
        print(json.dumps({'error': str(error)}))
        sys.exit(1)
