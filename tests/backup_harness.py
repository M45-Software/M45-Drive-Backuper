#!/usr/bin/python3
"""Run the real job adapter and Borg in a new, unprivileged backup laboratory.

Only OS boundaries (mount identity, free space, package inventory and selected
faults) are simulated. The generated shell control flow and Borg operations run.
No installed helper, systemd service, live configuration or real mount is used.
"""
import argparse
import datetime as dt
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import traceback
from unittest.mock import patch

PROJECT = Path(__file__).resolve().parents[1]
FIXTURE = Path(__file__).parent / 'fixtures/local-system-backup.sh'
UUID = 'aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee'
SOURCE_UUID = '11111111-2222-3333-4444-555555555555'
MARKER = 'M45 isolated backup harness v1\n'


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def load_backend():
    sys.path.insert(0, str(PROJECT / 'backend'))
    spec = importlib.util.spec_from_file_location('harness_backend', PROJECT / 'backend/m45-drive-backuper.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def event(root, **fields):
    with (root / 'events.jsonl').open('a') as stream:
        stream.write(json.dumps(fields) + '\n')


def inside(root, value):
    path = Path(value).resolve()
    require(path.is_relative_to(root), 'Refusing a path outside the test directory: ' + str(path))
    return path


def command(name, args):
    """Private command shims, invoked only by a job inside a marked test run."""
    require(os.geteuid() != 0, 'Run this harness as a normal user, without sudo.')
    root = Path(os.environ['M45_HARNESS_ROOT']).resolve()
    require((root / '.m45-harness').read_text() == MARKER, 'Missing test-directory marker')
    state = json.loads((root / 'state.json').read_text())
    log_json = name == 'borg' and args[:1] == ['--log-json']
    if log_json:
        args = args[1:]
    event(root, command=name, args=args, phase='start', log_json=log_json)
    if name == 'findmnt':
        require(args in (['-rn', '-M', str(root / 'destination'), '-o', 'UUID'],
                         ['-rn', '-M', str(root / 'source files'), '-o', 'UUID'],
                         ['-rn', '-T', '.', '-o', 'UUID']), 'Unexpected mount lookup')
        source = str(root / 'source files') in args
        print(state.get('source_uuid', SOURCE_UUID) if source else state.get('destination_uuid', UUID))
        return 0
    if name == 'df':
        require(args == ['-B1', '--output=avail', '.'], 'Unexpected free-space lookup')
        print('Avail\n' + str(state.get('free_bytes', 100 * 1024**3)))
        return 0
    if name == 'recovery-helper':
        require(args == ['recovery-refresh'], 'No privileged or installation actions are allowed')
        backend = load_backend()
        def inventory_command(argv):
            if state.get('inventory_failure'):
                raise ValueError('Injected package inventory failure')
            if argv == ['dpkg', '--print-architecture']:
                return 'amd64'
            if argv[0] == 'dpkg-query':
                return 'bash\t5.2\tinstalled\nexample-editor\t' + str(state['generation']) + '\tinstalled'
            if argv == ['apt-mark', 'showmanual']:
                return 'bash\nexample-editor'
            if argv == ['apt-mark', 'showhold']:
                return ''
            raise AssertionError('Unexpected inventory command: ' + repr(argv))
        # Patch paths/OS inputs in this child process only. This exercises the
        # actual recovery-refresh dispatch, inventory serializer and atomic write.
        with patch.multiple(backend, SCRIPT=root/'job.sh', EXCLUDES=root/'excludes',
                            INTERVAL=root/'interval', RECOVERY=root/'recovery/software-recovery.json'), \
             patch.object(backend.recovery, 'command', side_effect=inventory_command), \
             patch.object(backend.recovery, 'os_release', return_value={'ID':'test-os','VERSION_ID':'1'}), \
             patch.object(backend.recovery.shutil, 'which', return_value=None), \
             patch.object(backend.os, 'geteuid', return_value=0), \
             patch.object(sys, 'argv', ['test-helper', 'recovery-refresh']):
            try:
                print(json.dumps(backend.main()))
                return 0
            except ValueError as error:
                print(str(error), file=sys.stderr)
                return 1
    require(name == 'borg', 'Unexpected test command: ' + name)
    repository = inside(root, os.environ['BORG_REPO'])
    require(repository == root/'destination/repository', 'Unexpected repository')
    for key in ('BORG_CACHE_DIR', 'BORG_SECURITY_DIR'):
        inside(root, os.environ[key])
    require(args[0] in ('create', 'prune', 'compact', 'config'), 'Unexpected generated Borg operation')
    if args[0] == 'create':
        index = args.index('::home-{now:%Y-%m-%dT%H:%M:%S}')
        for source in args[index + 1:]:
            inside(root, source)
        inside(root, args[args.index('--exclude-from') + 1])
        # A controlled clock avoids archive-name collisions and makes retention
        # deterministic; every test archive is in a distinct retained hour.
        args[index] = '::' + state['archive']
        args[1:1] = ['--timestamp', state['timestamp']]
        if state.get('create_failure'):
            event(root, command='borg', operation='create', phase='finish', returncode=2, injected=True)
            print(json.dumps(dict(type='log_message', levelname='ERROR', message='Injected Borg create failure')), file=sys.stderr)
            return 2
    if state.get('cleanup_error') and args[0] == 'compact':
        print(json.dumps(dict(type='log_message', levelname='ERROR', message='Injected Borg compact failure')), file=sys.stderr)
        event(root, command='borg', operation='compact', phase='finish', returncode=2, injected=True)
        return 2
    result = subprocess.run(['/usr/bin/borg', *(['--log-json'] if log_json else []), *args], timeout=60)
    rc = result.returncode
    if state.get('cleanup_warning') and args[0] == 'compact' and rc == 0:
        print(json.dumps(dict(type='log_message', levelname='WARNING', message='Injected Borg compact warning')), file=sys.stderr)
        rc = 1
    event(root, command='borg', operation=args[0], phase='finish', returncode=rc)
    return rc


class Lab:
    def __init__(self, root):
        self.root = root.resolve()
        self.backend = load_backend()
        self.source = self.root/'source files'
        self.destination = self.root/'destination'
        self.repository = self.destination/'repository'
        self.system = self.root/'sample system'
        self.recovery = self.root/'recovery/software-recovery.json'
        self.results = []
        self.sequence = 0
        self.root.chmod(0o700)
        (self.root/'.m45-harness').write_text(MARKER)
        for path in (self.source, self.destination, self.system, self.root/'bin', self.root/'logs', self.root/'home'):
            path.mkdir()
        self.env = dict(PATH=str(self.root/'bin')+':/usr/bin:/bin', HOME=str(self.root/'home'),
                        LANG='C.UTF-8', LC_ALL='C.UTF-8', M45_HARNESS_ROOT=str(self.root),
                        BORG_CACHE_DIR=str(self.root/'cache'), BORG_SECURITY_DIR=str(self.root/'security'),
                        BORG_UNKNOWN_UNENCRYPTED_REPO_ACCESS_IS_OK='yes', BORG_PASSPHRASE='')
        for name in ('borg', 'findmnt', 'df', 'recovery-helper'):
            shim = self.root/'bin'/name
            shim.write_text('#!/bin/sh\nexec /usr/bin/python3 ' + shlex.quote(str(Path(__file__).resolve())) +
                            ' --command ' + shlex.quote(name) + ' "$@"\n')
            shim.chmod(0o700)
        self.files = {
            'Documents/keep.txt': b'First version of an important document.\n',
            "Documents/quotes ' and $ signs.txt": b'Literal path characters.\n',
            'Documents/café.txt': 'Small Unicode sample.\n'.encode(),
            'Documents/small.bin': bytes(range(256))*8,
            'Downloads/skip.txt': b'An excluded download.\n',
            'Downloads-keep/keep.txt': b'A similarly named folder stays included.\n',
            'project/code.py': b'print("keep source code")\n',
            'project/node_modules/dependency.js': b'Exclude downloaded dependencies.\n',
            'literal*?.txt': b'Exclude this exact filename.\n',
            'literalAB.txt': b'Do not interpret the literal filename as a glob.\n',
            'tagged-cache/CACHEDIR.TAG': b'Signature: 8a477f597d28d172789f06886806bc55\n',
            'tagged-cache/data.bin': b'Regenerable cached data.\n',
        }
        for name, data in self.files.items():
            path = self.source/name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        (self.source/'Documents/keep.txt').chmod(0o600)
        (self.source/'document-link').symlink_to('Documents/keep.txt')
        (self.system/'service.conf').write_text('example_setting=yes\n')
        prefix = str(self.source).lstrip('/')
        self.exclusions = f'# Sample selections\npp:{prefix}/Downloads\nsh:{prefix}/**/node_modules\npf:{prefix}/literal*?.txt\n'
        self.plan = dict(mountpoint=str(self.destination), repository=str(self.repository),
                         source=str(self.source), source_kind='drive', destination_kind='folder',
                         hours=6, hourly=365, daily=7, weekly=4, monthly=6, revision='a'*64,
                         compression='zstd,3', scan_mode='incremental', reserve_gib=10, quota_gib=-1,
                         exclude_caches=True, auto_mount=False, include_system=True, package_recovery=True,
                         excludes=self.exclusions)
        self.template = FIXTURE.read_text()
        self.borg('init', '--encryption=none', self.repository)

    def borg(self, *args, cwd=None):
        result = subprocess.run(['/usr/bin/borg', *map(str, args)], env=self.env, cwd=cwd or self.root,
                                capture_output=True, timeout=90)
        require(result.returncode == 0, result.stderr.decode(errors='replace'))
        return result.stdout

    def render(self, **options):
        self.plan.update(options)
        self.backend.validate(self.plan)
        with patch.object(self.backend.recovery, 'SYSTEM_PATHS', [str(self.system), str(self.root/'missing-system-path')]):
            self.template = self.backend.replacement(self.template, dict(self.plan, source_uuid=SOURCE_UUID))
        # Keep an unredirected adapter output for debugging and repeated saves.
        (self.root/'generated-original.sh').write_text(self.template)
        text = self.template
        resources = {
            '/run/lock/local-system-backup.lock': self.root/'job.lock',
            '/var/cache/local-home-backup': self.root/'cache',
            '/var/lib/local-home-backup/security': self.root/'security',
            '/etc/local-system-backup.exclude': self.root/'excludes',
            '/usr/libexec/m45-drive-backuper/backend': self.root/'bin/recovery-helper',
            '/var/lib/m45-drive-backuper/recovery': self.recovery.parent,
        }
        for original, local in resources.items():
            text = text.replace(original, shlex.quote(str(local)))
        text = text.replace('export PATH=/usr/sbin:/usr/bin:/sbin:/bin',
                            'export PATH=' + shlex.quote(self.env['PATH']))
        require('/home/example' not in text and '/media/example' not in text, 'Unredirected fixture source/destination')
        (self.root/'job.sh').write_text(text)
        (self.root/'excludes').write_text(self.plan['excludes'])
        (self.root/'interval').write_text('[Timer]\nOnCalendar=*-*-* 00/6:00:00\n')
        subprocess.run(['/bin/bash', '-n', str(self.root/'job.sh')], check=True)

    def run_job(self, name, **faults):
        self.sequence += 1
        timestamp = (dt.datetime(2026, 1, 1, tzinfo=dt.timezone.utc) + dt.timedelta(hours=self.sequence)).isoformat()
        archive = 'home-' + timestamp[:19]
        state = dict(archive=archive, timestamp=timestamp, generation=self.sequence, **faults)
        (self.root/'state.json').write_text(json.dumps(state))
        (self.root/'events.jsonl').write_text('')
        result = subprocess.run(['/bin/bash', str(self.root/'job.sh')], cwd=self.root, env=self.env,
                                capture_output=True, text=True, timeout=90)
        log = self.root/'logs'/name
        log.with_suffix('.stdout').write_text(result.stdout)
        log.with_suffix('.stderr').write_text(result.stderr)
        events = [json.loads(line) for line in (self.root/'events.jsonl').read_text().splitlines()]
        log.with_suffix('.events.json').write_text(json.dumps(events, indent=2))
        report, readable = self.backend.reporting.parse(result.stderr)
        log.with_suffix('.borg-report.json').write_text(json.dumps(report, indent=2))
        log.with_suffix('.activity.txt').write_text(readable)
        self.results.append(dict(name=name, job_exit=result.returncode, archive=archive,
                                 borg_create_exit=next((e['returncode'] for e in events if e.get('operation') == 'create'), None),
                                 borg_report=report,
                                 status='checking'))
        return result, archive, events

    def passed(self):
        self.results[-1]['status'] = 'passed'
        print('PASS ' + self.results[-1]['name'], flush=True)

    def names(self):
        return [a['name'] for a in json.loads(self.borg('list', '--json', self.repository))['archives']]

    def paths(self, archive):
        return self.borg('list', '--format', '{path}{NL}', f'{self.repository}::{archive}').decode().splitlines()

    def extract(self, archive, path):
        return self.borg('extract', '--stdout', f'{self.repository}::{archive}', str(path).lstrip('/'))

    def recovery_manifest(self, archive):
        require(str(self.recovery).lstrip('/') in self.paths(archive), 'Recovery file was not included in the archive')
        manifest = json.loads(self.extract(archive, self.recovery))
        require(manifest['schema'] == self.backend.recovery.SCHEMA and manifest['apt_manual'] == ['bash','example-editor'], 'Recovery manifest missing or invalid')
        return manifest

    def no_backup(self, name, **faults):
        before = self.names()
        result, _, events = self.run_job(name, **faults)
        require(result.returncode != 0, name + ' did not stop the job')
        require(not any(e['command'] == 'borg' for e in events), name + ' reached cleanup or archive creation')
        require(self.names() == before, name + ' changed saved snapshots')
        require(self.results[-1]['borg_report']['outcome'] == 'error', 'Preflight failure misclassified as a Borg warning')
        self.passed()

    def exercise(self):
        self.render()
        result, first, events = self.run_job('01-backup-and-restore')
        require(result.returncode == 0, result.stderr)
        require(self.results[-1]['borg_report']['outcome'] == 'success', 'Clean backup was not reported as success')
        paths = self.paths(first)
        skipped = ('Downloads/skip.txt', 'project/node_modules/dependency.js', 'literal*?.txt',
                   'tagged-cache/CACHEDIR.TAG', 'tagged-cache/data.bin')
        for name, content in self.files.items():
            present = str(self.source/name).lstrip('/') in paths
            require(present == (name not in skipped), 'Incorrect inclusion: ' + name)
            if present:
                require(self.extract(first, self.source/name) == content, 'Restored bytes differ: ' + name)
        require(self.extract(first, self.system/'service.conf') == b'example_setting=yes\n', 'System settings missing')
        manifest = self.recovery_manifest(first)
        require(self.recovery.stat().st_mode & 0o777 == 0o600, 'Recovery file permissions changed')
        commands = [e['command'] for e in events if e['phase'] == 'start']
        require(commands.index('recovery-helper') < commands.index('borg'), 'Recovery refresh ran after cleanup')
        restore = self.root/'restored'
        restore.mkdir()
        self.borg('extract', f'{self.repository}::{first}', cwd=restore)
        restored_source = restore/str(self.source).lstrip('/')
        require((restored_source/'Documents/keep.txt').stat().st_mode & 0o777 == 0o600, 'Restored mode changed')
        require(os.readlink(restored_source/'document-link') == 'Documents/keep.txt', 'Restored symlink changed')
        self.passed()

        (self.source/'Documents/keep.txt').write_bytes(b'Second version.\n')
        (self.source/'Documents/new.txt').write_bytes(b'New file.\n')
        result, second, _ = self.run_job('02-changes-and-fresh-recovery')
        require(result.returncode == 0, result.stderr)
        require(self.extract(first, self.source/'Documents/keep.txt') == self.files['Documents/keep.txt'], 'Earlier snapshot changed')
        require(self.extract(second, self.source/'Documents/keep.txt') == b'Second version.\n', 'Changed file not backed up')
        require(self.extract(second, self.source/'Documents/new.txt') == b'New file.\n', 'New file missing')
        fresh = self.recovery_manifest(second)
        require(fresh['apt_installed'][1]['version'] == '2' and fresh['generated_at'] != manifest['generated_at'], 'Recovery was not refreshed')
        self.passed()

        previous_recovery = self.recovery.read_bytes()
        self.render(package_recovery=False, include_system=False)
        result, disabled, events = self.run_job('03-disable-recovery-and-system-settings')
        require(result.returncode == 0, result.stderr)
        require(not any(e['command'] == 'recovery-helper' for e in events), 'Disabled recovery was refreshed')
        paths = self.paths(disabled)
        require(str(self.recovery).lstrip('/') not in paths, 'Disabled recovery is still included')
        require(str(self.system/'service.conf').lstrip('/') not in paths, 'Disabled system settings still included')
        require(self.recovery.read_bytes() == previous_recovery, 'Disabling recovery modified the existing file')
        require(self.extract(first, self.recovery), 'Disabling recovery removed earlier recovery data')
        self.passed()

        self.render(excludes='', exclude_caches=False, scan_mode='full_scan', compression='none', quota_gib=1)
        result, included, events = self.run_job('04-include-again-and-save-options')
        require(result.returncode == 0, result.stderr)
        for name in skipped:
            require(self.extract(included, self.source/name) == self.files[name], 'Include-again failed: ' + name)
        args = next(e['args'] for e in events if e['command'] == 'borg' and e['phase'] == 'start' and e['args'][0] == 'create')
        require(args[args.index('--compression')+1] == 'none' and args[args.index('--files-cache')+1] == 'disabled', 'Options did not reach Borg')
        require(int(self.borg('config', self.repository, 'storage_quota')) == 1024**3, 'Quota was not applied')
        self.passed()

        self.render(package_recovery=True, include_system=True, excludes=self.exclusions,
                    exclude_caches=True, scan_mode='incremental', compression='zstd,3', quota_gib=0)
        self.no_backup('05-wrong-destination-uuid', destination_uuid='wrong-drive')
        self.no_backup('06-missing-destination', destination_uuid='')
        self.no_backup('07-wrong-source-uuid', source_uuid='wrong-source')
        self.no_backup('08-recovery-generation-failure', inventory_failure=True)
        moved = self.root/'temporarily-missing-source'
        self.source.rename(moved)
        try:
            self.no_backup('09-missing-source-folder')
        finally:
            moved.rename(self.source)

        before = self.names()
        with (self.root/'job.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result, _, events = self.run_job('10-busy-job-lock')
        require(result.returncode == 0 and not any(e['command'] == 'borg' for e in events), 'Busy job did not skip safely')
        require(self.names() == before, 'Busy job created an archive')
        require(self.results[-1]['borg_report']['outcome'] == 'skipped', 'Skipped backup incorrectly reported as completed')
        self.results[-1]['note'] = 'Existing job policy skips a held job lock with exit 0; no backup ran.'
        self.passed()

        result, _, events = self.run_job('11-low-free-space', free_bytes=0)
        require(result.returncode != 0 and not any(e.get('operation') == 'create' for e in events), 'Low-space guard failed')
        require(self.names() == before, 'Low-space fixture unexpectedly lost retained snapshots')
        self.passed()

        result, _, events = self.run_job('12-borg-create-failure', create_failure=True)
        require(result.returncode == 2, 'Borg error was hidden')
        require(self.results[-1]['borg_report']['outcome'] == 'error', 'Borg failure not reported')
        require(any('Injected Borg create failure' in i['message'] for i in self.results[-1]['borg_report']['issues']), 'Borg error message missing')
        require(events[-1].get('operation') == 'create', 'Cleanup ran after a failed create')
        require(self.names() == before, 'Failed create changed retained snapshots')
        self.passed()

        unreadable = self.source/'unreadable-test-file'
        unreadable.write_text('This deliberately unreadable sample must cause a Borg warning.\n')
        unreadable.chmod(0)
        try:
            result, warning, events = self.run_job('13-borg-warning-reporting')
        finally:
            unreadable.chmod(0o600)
        require(self.results[-1]['borg_create_exit'] == 1, 'Expected a genuine Borg warning for the unreadable sample')
        require(warning in self.names(), 'Borg warning did not produce an archive')
        require(result.returncode == 1, 'Borg warning was converted to success')
        require(self.results[-1]['borg_report']['outcome'] == 'warning', 'Borg warning not reported')
        require(any('unreadable-test-file' in i['message'] and i['level'] == 'WARNING'
                    for i in self.results[-1]['borg_report']['issues']), 'Actual Borg warning message missing')
        self.passed()

        result, _, _ = self.run_job('14-cleanup-warning', cleanup_warning=True)
        require(result.returncode == 1 and self.results[-1]['borg_create_exit'] == 0, 'Maintenance warning stopped the backup or disappeared')
        require(self.results[-1]['borg_report']['outcome'] == 'warning', 'Cleanup warning hidden by successful create')
        self.passed()

        result, _, _ = self.run_job('15-cleanup-error', cleanup_error=True)
        require(result.returncode == 2 and self.results[-1]['borg_create_exit'] is None, 'Backup continued after cleanup error')
        require(self.results[-1]['borg_report']['outcome'] == 'error', 'Cleanup error not reported')
        self.passed()

        result, _, _ = self.run_job('16-clean-run-clears-previous-warning')
        require(result.returncode == 0, result.stderr)
        require(self.results[-1]['borg_report']['outcome'] == 'success' and not self.results[-1]['borg_report']['issues'], 'Previous attempt diagnostics leaked into the new run')
        self.passed()

        self.borg('check', '--verify-data', self.repository)
        self.borg('extract', '--dry-run', f'{self.repository}::{second}')
        self.results.append(dict(name='17-borg-integrity-and-extraction-check', status='passed'))
        print('PASS 17-borg-integrity-and-extraction-check', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=PROJECT/'build/backup-lab', help='Parent directory for a new retained run; existing runs are never overwritten')
    parser.add_argument('--temporary', action='store_true', help='Remove this run after testing (for CTest)')
    parser.add_argument('--strict', action='store_true', help='Retained for compatibility; all gaps now fail every run')
    args = parser.parse_args()
    require(os.geteuid() != 0, 'Run this harness as a normal user, without sudo.')
    require(Path('/usr/bin/borg').is_file(), 'Install Borg 1.2 to run the backup harness.')
    version = subprocess.check_output(['/usr/bin/borg', '--version'], text=True).strip()
    require(version.startswith('borg 1.2.'), 'This fixture targets Borg 1.2; found ' + version)
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='run-', dir=args.output)).resolve()
    print('Backup laboratory: ' + str(root), flush=True)
    lab = None
    failed = None
    try:
        lab = Lab(root)
        lab.exercise()
    except Exception:
        failed = traceback.format_exc()
        print(failed, file=sys.stderr)
        if lab and lab.results and lab.results[-1]['status'] == 'checking':
            lab.results[-1]['status'] = 'failed'
    results = lab.results if lab else []
    gaps = [r for r in results if r['status'] == 'known-gap']
    report = dict(borg_version=version, directory=str(root), checks=results, failure=failed,
                  known_gaps=gaps, real_operations=['Borg init/create/prune/compact/config/list/extract/check',
                  'generated shell job', 'package recovery serialization and atomic write', 'job flock', 'Borg diagnostic parsing'],
                  simulated=['mount UUIDs', 'free-space readings', 'package-manager inventory',
                             'archive clock', 'fatal Borg create error', 'cleanup warning/error'],
                  not_exercised=['KDE UI/authentication/notifications', 'physical mounting', 'systemd timer execution', 'package reinstallation'])
    (root/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    summary = '\n'.join(f"{r['status'].upper()}: {r['name']}" + (': '+r['note'] if r.get('note') else '') for r in results)
    (root/'REPORT.txt').write_text(version+'\n'+summary+'\n'+(failed or '')+'\nSee report.json for scope and logs/ for each generated-job run.\n')
    print(summary)
    print('Report: ' + str(root/'REPORT.txt'), flush=True)
    if args.temporary:
        shutil.rmtree(root)
    return 1 if failed or (args.strict and gaps) else 0


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == '--command':
        sys.exit(command(sys.argv[2], sys.argv[3:]))
    sys.exit(main())
