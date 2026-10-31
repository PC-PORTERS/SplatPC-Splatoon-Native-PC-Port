"""Portable checks (Linux and Windows) for the 288 update scripts and the manifests they read."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / 'scripts'


def text(path):
    return Path(path).read_text(encoding='utf-8')


def identity(manifest):
    return dict(re.findall(r'^(\w+)\s*=\s*(\S+)', text(manifest).split('[identity]', 1)[1].split('\n[', 1)[0], re.M))


class UpdateScriptTests(unittest.TestCase):
    def test_only_the_expected_scripts_exist(self):
        names = sorted(p.name for p in SCRIPTS.glob('*.ps1'))
        self.assertEqual(names, ['Install-Update.ps1', 'Setup.ps1'])

    def test_update_build_lives_in_the_installer(self):
        installer = text(SCRIPTS / 'Install-Update.ps1')
        self.assertIn('function Invoke-UpdateBuild', installer)
        self.assertRegex(installer, r'--manifest \$Manifest')
        self.assertRegex(installer, r'-DRBREW_MANIFEST=\$Manifest')
        self.assertIn('$CopyDependencyGame=', installer)
        self.assertNotIn("CopyTree (Join-Path $DependencyRoot 'Game\\mlc01", installer)

    def test_setup_routes_an_update_image_by_its_manifest(self):
        setup = text(SCRIPTS / 'Setup.ps1')
        self.assertIn('Test-UpdateManifest', setup)
        self.assertIn("Install-Update.ps1", setup)

    def test_no_profile_folder_or_removed_script_is_referenced(self):
        for path in list(SCRIPTS.glob('*.ps1')) + [ROOT / 'Setup.bat', ROOT / 'README.md', ROOT / 'docs/update-288.md']:
            body = text(path)
            for stale in ('profiles\agmp-v288', 'profiles/agmp-v288', 'Build-Update.ps1', 'Launch-Update.ps1'):
                self.assertFalse(stale in body, f'{path.name} mentions {stale}')
        self.assertFalse((ROOT / 'profiles').exists())

    def test_installer_digests_come_from_the_manifests(self):
        base = identity(ROOT / 'Manifest/Gambit_v16.manifest')
        update = identity(ROOT / 'Manifest/Gambit_v288.manifest')
        self.assertEqual(len(base['image_sha256']), 64)
        self.assertEqual(len(update['image_sha256']), 64)
        self.assertNotEqual(base['image_sha256'], update['image_sha256'])
        self.assertEqual(update['app_version'], '288')
        self.assertTrue(update['title_id'].upper().startswith('0X0005000E'))
        installer = text(SCRIPTS / 'Install-Update.ps1')
        self.assertIn('Resolve-UpdateImage', installer)
        self.assertNotIn('Gambit_v288', installer)
        self.assertNotIn('Splatoon-v288', installer)


if __name__ == '__main__':
    unittest.main()
