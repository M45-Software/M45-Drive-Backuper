"""Portable software inventory and deliberately reviewed APT reinstallation."""
import datetime
import json
import os
from pathlib import Path
import pwd
import re
import shlex
import shutil
import subprocess

SCHEMA = 'm45-software-recovery-v1'
SYSTEM_PATHS = ['/etc', '/usr/local', '/root', '/var/spool/cron', '/var/lib/dpkg',
                '/var/lib/apt/extended_states', '/var/lib/NetworkManager',
                '/var/lib/bluetooth', '/var/lib/AccountsService']
PACKAGE_NAME = re.compile(r'[a-z0-9][a-z0-9+.-]*(?::[a-z0-9][a-z0-9-]*)?\Z')


def os_release():
    result = {}
    for line in Path('/etc/os-release').read_text().splitlines():
        if '=' in line:
            key, value = line.split('=', 1)
            if key in ('ID', 'VERSION_ID', 'PRETTY_NAME'):
                result[key] = shlex.split(value)[0]
    return result


def command(args):
    result = subprocess.run(args, text=True, capture_output=True, timeout=90,
                            env=dict(os.environ, PATH='/usr/sbin:/usr/bin:/sbin:/bin', LC_ALL='C'))
    if result.returncode:
        raise ValueError(result.stderr.strip() or result.stdout.strip() or 'Inventory command failed: ' + args[0])
    return result.stdout.strip()


def inventory(source):
    result = dict(schema=SCHEMA, generated_at=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  os=os_release(), architecture=command(['dpkg', '--print-architecture']), warnings=[])
    rows = command(['dpkg-query', '-W', '-f=${binary:Package}\t${Version}\t${db:Status-Status}\n'])
    result['apt_installed'] = [dict(name=p[0], version=p[1]) for line in rows.splitlines()
                               if len(p := line.split('\t')) == 3 and p[2] == 'installed']
    installed = {p['name'] for p in result['apt_installed']}
    installed.update(name.split(':')[0] for name in installed.copy())
    result['apt_manual'] = sorted(p for p in command(['apt-mark', 'showmanual']).splitlines() if p in installed)
    result['apt_hold'] = command(['apt-mark', 'showhold']).splitlines()
    for key, args in {
        'services_enabled': ['systemctl', 'list-unit-files', '--state=enabled', '--type=service', '--no-legend', '--no-pager'],
        'timers_enabled': ['systemctl', 'list-unit-files', '--state=enabled', '--type=timer', '--no-legend', '--no-pager'],
        'snaps': ['snap', 'list'],
        'flatpak_system': ['flatpak', '--system', 'list', '--app', '--columns=application,arch,branch,origin'],
        'flatpak_remotes_system': ['flatpak', '--system', 'remotes', '--columns=name,url'],
    }.items():
        if not shutil.which(args[0]):
            result[key] = ''
            continue
        try:
            result[key] = command(args)
        except (ValueError, subprocess.TimeoutExpired) as error:
            result[key] = ''
            result['warnings'].append(key + ': ' + str(error))
    try:
        owner = pwd.getpwuid(Path(source).stat().st_uid)
        if owner.pw_uid and shutil.which('flatpak'):
            result['flatpak_user_account'] = owner.pw_name
            args = ['flatpak', '--user', 'list', '--app', '--columns=application,arch,branch,origin']
            if os.geteuid() == 0:
                args = ['runuser', '-u', owner.pw_name, '--', 'env', 'HOME=' + owner.pw_dir] + args
            elif os.geteuid() != owner.pw_uid:
                raise ValueError('Source owner differs from the current user; collect that user’s Flatpak inventory separately.')
            result['flatpak_user'] = command(args)
    except (KeyError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        result['warnings'].append('User Flatpak inventory: ' + str(error))
    result['instructions'] = [
        'Open this JSON file with Reinstall from recovery file in M45 Drive Backuper.',
        'APT restoration installs manually selected packages and their dependencies from your currently configured repositories.',
        'Use the same distribution, release and architecture. Versions listed here are reference information, not pinned versions.',
        'Review and restore repository sources and signing keys from your /etc backup before reinstalling third-party packages.',
        'Snap and Flatpak inventories, held packages, services and timers are reference lists; review and restore those separately.',
        'User files, passwords, service settings, databases and package binaries are not stored in this recovery file.',
    ]
    return result


def validate_manifest(data):
    if not isinstance(data, dict) or data.get('schema') != SCHEMA:
        raise ValueError('Choose an M45 software recovery JSON file.')
    current = os_release()
    saved = data.get('os')
    if not isinstance(saved, dict) or any(saved.get(key) != current.get(key) for key in ('ID', 'VERSION_ID')):
        raise ValueError('This recovery file is for a different operating-system release. Package installation was not started.')
    if data.get('architecture') != command(['dpkg', '--print-architecture']):
        raise ValueError('This recovery file is for a different processor architecture.')
    packages = data.get('apt_manual')
    if not isinstance(packages, list) or not packages or len(packages) > 20000:
        raise ValueError('The recovery file needs a nonempty list of manually selected APT packages.')
    if any(not isinstance(p, str) or not PACKAGE_NAME.fullmatch(p) or len(p) > 200 for p in packages):
        raise ValueError('The recovery file contains an invalid package name.')
    return sorted(set(packages))


def restore(data, apply=False):
    packages = validate_manifest(data)
    preview = command(['apt-get', '--simulate', '--no-remove', 'install', '--'] + packages)
    if any(line.startswith('Remv ') for line in preview.splitlines()):
        raise ValueError('The requested packages would remove installed software. Installation was refused.')
    if not apply:
        return dict(message='Review the package-manager plan before installing.', preview=preview, package_count=len(packages))
    if os.geteuid() != 0:
        raise PermissionError('Installing packages requires administrator authentication.')
    output = subprocess.run(['apt-get', '--assume-yes', '--no-remove', 'install', '--'] + packages,
                            text=True, capture_output=True, timeout=3600,
                            env=dict(os.environ, PATH='/usr/sbin:/usr/bin:/sbin:/bin', LC_ALL='C', DEBIAN_FRONTEND='noninteractive'))
    if output.returncode:
        raise ValueError('Package installation did not finish. Some packages may already have changed.\n' + output.stdout[-12000:] + '\n' + output.stderr[-4000:])
    return dict(message='APT packages restored. Review the saved Snap, Flatpak, held-package and service lists separately.', output=output.stdout)
