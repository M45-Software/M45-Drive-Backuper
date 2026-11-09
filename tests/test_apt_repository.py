import hashlib
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


publisher = module('apt_publisher', ROOT / 'scripts/publish-apt.py')


@unittest.skipUnless(all(shutil.which(tool) for tool in ['gpg', 'gpgv', 'dpkg-deb', 'dpkg-scanpackages']), 'APT authoring tools are required')
class RepositoryTests(unittest.TestCase):
    def test_signed_archive_and_immutable_package_versions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            home = root / 'gnupg'
            home.mkdir(mode=0o700)
            try:
                subprocess.run(['gpg', '--homedir', str(home), '--batch', '--pinentry-mode', 'loopback', '--passphrase', '',
                                '--quick-generate-key', 'M45 Test Archive', 'ed25519', 'sign', '1d'], check=True, capture_output=True)
                keys = subprocess.check_output(['gpg', '--homedir', str(home), '--batch', '--with-colons', '--list-secret-keys']).decode()
                fingerprint = next(line.split(':')[9] for line in keys.splitlines() if line.startswith('fpr:'))
                fixture = root / 'fixture'
                (fixture / 'DEBIAN').mkdir(parents=True)
                (fixture / 'DEBIAN/control').write_text('Package: m45-test\nVersion: 1.0\nArchitecture: amd64\nMaintainer: M45 Test\nDescription: repository fixture\n')
                package = root / 'm45-test.deb'
                subprocess.run(['dpkg-deb', '--root-owner-group', '--build', str(fixture), str(package)], check=True, capture_output=True)
                repository = root / 'repository'
                publisher.publish(repository, home, fingerprint, [package])
                public = repository / 'm45-software.asc'
                trusted = root / 'trusted.gpg'
                subprocess.run(['gpg', '--batch', '--dearmor', '--output', str(trusted), str(public)], check=True)
                subprocess.run(['gpgv', '--keyring', str(trusted), str(repository / 'dists/stable/InRelease')], check=True, capture_output=True)
                damaged = root / 'tampered-InRelease'
                damaged.write_bytes((repository / 'dists/stable/InRelease').read_bytes().replace(b'Origin: M45 Software', b'Origin: Changed'))
                check = subprocess.run(['gpgv', '--keyring', str(trusted), str(damaged)], capture_output=True)
                self.assertNotEqual(check.returncode, 0)
                index = repository / 'dists/stable/main/binary-amd64/Packages'
                self.assertIn('Filename: pool/main/m45-test_1.0_amd64.deb', index.read_text())
                digest = hashlib.sha256(index.read_bytes()).hexdigest()
                old_index = index.parent / 'by-hash/SHA256' / digest
                self.assertEqual(old_index.read_bytes(), index.read_bytes())
                control = fixture / 'DEBIAN/control'
                control.write_text(control.read_text().replace('Version: 1.0', 'Version: 1.1'))
                subprocess.run(['dpkg-deb', '--root-owner-group', '--build', str(fixture), str(package)], check=True, capture_output=True)
                publisher.publish(repository, home, fingerprint, [package])
                self.assertTrue(old_index.exists())
                self.assertNotEqual(digest, hashlib.sha256(index.read_bytes()).hexdigest())
                with self.assertRaisesRegex(ValueError, 'existing package version cannot change'):
                    control.write_text(control.read_text().replace('repository fixture', 'changed fixture'))
                    subprocess.run(['dpkg-deb', '--root-owner-group', '--build', str(fixture), str(package)], check=True, capture_output=True)
                    publisher.publish(repository, home, fingerprint, [package])
                with self.assertRaises(ValueError):
                    publisher.publish(repository, repository / 'secret', fingerprint, [])
            finally:
                subprocess.run(['gpgconf', '--homedir', str(home), '--kill', 'gpg-agent'], capture_output=True)
