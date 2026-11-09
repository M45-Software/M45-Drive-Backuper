"""Structured Borg diagnostics scoped to one systemd service invocation."""
import json
import re
from datetime import datetime, timezone

# Injected once into the supported job. No new privileged command or status file
# is needed: systemd captures these messages with its invocation ID.
SHELL = r'''# M45 Borg reporting
m45_borg_rc=-1
m45_create_rc=-1
m45_finished=0
m45_report_exit() {
    local rc=$?
    trap - EXIT
    if (( m45_borg_rc > rc )); then rc=$m45_borg_rc; fi
    printf 'M45_BACKUP_RESULT {"exit_code":%d,"borg_exit_code":%d,"create_exit_code":%d,"finished":%d}\n' "$rc" "$m45_borg_rc" "$m45_create_rc" "$m45_finished" >&2
    exit "$rc"
}
trap m45_report_exit EXIT
borg() {
    local rc=0
    # An absent quota is an expected negative result for this read-only probe.
    if [[ $# == 3 && "$1" == config && "$3" == storage_quota ]]; then
        command borg "$@"
        return $?
    fi
    printf 'M45_BORG_BEGIN %s\n' "$1" >&2
    command borg --log-json "$@" || rc=$?
    printf 'M45_BORG_RESULT %s %d\n' "$1" "$rc" >&2
    if (( rc > m45_borg_rc )); then m45_borg_rc=$rc; fi
    if [[ "$1" == create ]]; then m45_create_rc=$rc; fi
    # Maintenance warnings permit the backup to continue, but the EXIT trap
    # retains the warning even if a later command succeeds.
    if (( rc == 1 )) && [[ "$1" != create ]]; then return 0; fi
    return "$rc"
}
# End M45 Borg reporting
'''


def upgrade_script(text):
    text = re.sub(r'^# M45 Borg reporting\n.*?^# End M45 Borg reporting\n', '', text, flags=re.M | re.S)
    text = text.replace('m45_finished=1\nexit "$backup_rc"', 'exit "$backup_rc"')
    # Preserve other exit traps rather than silently overriding custom jobs.
    if re.search(r'^\s*trap\b[^\n]*(?:\bEXIT\b|\s0(?:\s|$))', text, re.M):
        raise ValueError('This backup job has a custom exit trap; reporting cannot safely replace it.')
    if re.search(r'^\s*(?:function\s+borg\b|borg\s*\(\))', text, re.M):
        raise ValueError('This backup job has a custom Borg wrapper; reporting cannot safely replace it.')
    if len(re.findall(r'^set -euo pipefail$', text, re.M)) != 1 or text.count('exit "$backup_rc"') != 1:
        raise ValueError('Unsupported backup result handling; no changes were made.')
    text = text.replace('set -euo pipefail\n', 'set -euo pipefail\n' + SHELL, 1)
    text = text.replace('exit "$backup_rc"', 'm45_finished=1\nexit "$backup_rc"')
    # Remove only the recognized legacy warning-to-success assignment. The
    # aggregate trap also protects against maintenance warnings being masked.
    text = re.sub(r'(if \(\( backup_rc == 1 \)\); then\n\s*echo [^\n]+\n)\s*backup_rc=0\n', r'\1', text)
    text = text.replace('# Keep warnings in the journal without triggering the systemd failure alert.',
                        '# Preserve Borg warning status for the monitor and system-service alert.')
    return text


def parse(messages):
    """Parse output from one invocation; unknown/plain output stays readable."""
    report, readable, _ = parse_records([dict(message=line) for line in messages.splitlines()])
    return report, readable


def parse_journal(messages):
    """Keep journal timestamps and priorities alongside the readable Borg output."""
    records = []
    for line in messages.splitlines():
        try:
            entry = json.loads(line)
        except ValueError:
            entry = None
        if not isinstance(entry, dict) or not isinstance(entry.get('MESSAGE'), str):
            records.append(dict(message=line))
            continue
        timestamp = ''
        try:
            timestamp = datetime.fromtimestamp(int(entry['__REALTIME_TIMESTAMP']) / 1_000_000,
                                              timezone.utc).isoformat(timespec='seconds')
        except (KeyError, ValueError, TypeError, OverflowError, OSError):
            pass
        priority = str(entry.get('PRIORITY', '6'))
        level = {'0': 'CRITICAL', '1': 'CRITICAL', '2': 'CRITICAL', '3': 'ERROR',
                 '4': 'WARNING', '5': 'INFO', '6': 'INFO', '7': 'DEBUG'}.get(priority, 'INFO')
        for message in entry['MESSAGE'].splitlines():
            records.append(dict(message=message, timestamp=timestamp, level=level))
    return parse_records(records)


def parse_records(records):
    report = dict(outcome='unknown', issues=[], operations=[], result=None)
    readable = []
    operation = ''
    activity = []
    for record in records:
        line = record['message']

        def emit(text, level=None, source='Service', message=None):
            readable.append(text)
            activity.append(dict(timestamp=record.get('timestamp', ''),
                                 level=level or record.get('level', 'INFO'), source=source,
                                 message=message if message is not None else text, raw=text))
        if line.startswith('M45_BORG_BEGIN '):
            operation = line.removeprefix('M45_BORG_BEGIN ')
            emit('Borg ' + operation + ' started', 'INFO', 'Borg ' + operation, 'Started')
            continue
        match = re.fullmatch(r'M45_BORG_RESULT ([a-z-]+) (\d+)', line)
        if match:
            name, rc = match[1], int(match[2])
            report['operations'].append(dict(operation=name, exit_code=rc))
            emit(f'Borg {name} exited {rc}', 'INFO' if rc == 0 else 'WARNING' if rc == 1 else 'ERROR',
                 'Borg ' + name, f'Finished · exit code {rc}')
            operation = ''
            continue
        if line.startswith('M45_BACKUP_RESULT '):
            try:
                result = json.loads(line.removeprefix('M45_BACKUP_RESULT '))
                if (isinstance(result, dict) and all(type(result.get(k)) is int for k in
                        ('exit_code', 'borg_exit_code', 'create_exit_code', 'finished'))
                        and 0 <= result['exit_code'] <= 255 and -1 <= result['borg_exit_code'] <= 255
                        and -1 <= result['create_exit_code'] <= 255 and result['finished'] in (0, 1)):
                    report['result'] = result
                    emit('Backup job exited ' + str(result['exit_code']),
                         'INFO' if result['exit_code'] == 0 else 'WARNING' if (result['exit_code'] == 1
                         and result['borg_exit_code'] == 1 and result['create_exit_code'] in (0, 1)
                         and result['finished']) else 'ERROR', 'Backup job')
                    continue
            except (ValueError, TypeError):
                pass
        try:
            entry = json.loads(line)
        except ValueError:
            entry = None
        if isinstance(entry, dict) and entry.get('type') == 'log_message' and isinstance(entry.get('message'), str):
            level = entry.get('levelname', 'INFO')
            if level not in ('DEBUG', 'INFO', 'WARNING', 'ERROR', 'CRITICAL'):
                level = 'INFO'
            message = entry['message']
            emit(f'{level} · Borg {operation}: {message}', level, ('Borg ' + operation).strip(), message)
            if level in ('WARNING', 'ERROR', 'CRITICAL'):
                report['issues'].append(dict(level=level, operation=operation, message=message))
        else:
            emit(line)
    result = report['result']
    if result:
        rc, borg_rc, create_rc = (result[k] for k in ('exit_code', 'borg_exit_code', 'create_exit_code'))
        if rc == 0:
            report['outcome'] = 'success' if result['finished'] and create_rc == 0 else 'skipped'
        elif rc == 1 and borg_rc == 1 and create_rc in (0, 1) and result['finished']:
            report['outcome'] = 'warning'
        else:
            report['outcome'] = 'error'
    elif report['issues'] or report['operations']:
        report['outcome'] = 'incomplete'
    return report, '\n'.join(readable), activity
