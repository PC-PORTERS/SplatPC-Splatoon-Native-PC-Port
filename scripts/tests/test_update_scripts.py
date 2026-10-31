"""Portable checks for the merged-ROM workflow and the manifests it relies on."""

import importlib.util
import re
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / 'scripts'
TOOL = SCRIPTS / 'update_combinator.py'


def text(path):
    return Path(path).read_text(encoding='utf-8')


def identity(manifest):
    return dict(re.findall(r'^(\w+)\s*=\s*(\S+)', text(manifest).split('[identity]', 1)[1].split('\n[', 1)[0], re.M))


def load_tool():
    spec = importlib.util.spec_from_file_location('update_combinator', TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_dump(root, version, files):
    for folder in ('code', 'content', 'meta'):
        (root / folder).mkdir(parents=True)
    (root / 'code' / 'Gambit.rpx').write_bytes(f'rpx {version}'.encode())
    (root / 'meta' / 'meta.xml').write_text(f'<menu><title_version>{version}</title_version></menu>')
    for name, body in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body)


class UpdateCombinatorTests(unittest.TestCase):
    def test_only_setup_powershell_script_remains(self):
        names = sorted(p.name for p in SCRIPTS.glob('*.ps1'))
        self.assertEqual(names, ['Setup.ps1'])
        self.assertFalse((SCRIPTS / 'Install-Update.ps1').exists())

    def test_setup_scripts_use_the_merged_rom_directly(self):
        for name in ('Setup.bat', 'setup.sh', 'scripts/Setup.ps1'):
            body = text(ROOT / name)
            self.assertNotIn('Install-Update.ps1', body, name)
            self.assertNotIn('Test-UpdateManifest', body, name)
        setup_bat = text(ROOT / 'Setup.bat')
        self.assertIn(r'scripts\Setup.ps1', setup_bat)
        self.assertNotIn('update dump', setup_bat.lower())
        setup = text(SCRIPTS / 'Setup.ps1')
        self.assertNotIn('Splatoon-v', setup)

    def test_readme_and_docs_describe_the_merged_rom(self):
        readme = text(ROOT / 'README.md')
        self.assertIn('update_combinator.py', readme)
        self.assertIn('Update-combinator', readme)
        self.assertNotIn('Install-Update.ps1', readme)
        for doc in (ROOT / 'docs').glob('*.md'):
            self.assertNotIn('Install-Update.ps1', text(doc), doc.name)

    def test_manifests_still_describe_both_images(self):
        base = identity(ROOT / 'Manifest/Gambit_v16.manifest')
        update = identity(ROOT / 'Manifest/Gambit_v288.manifest')
        self.assertEqual(len(base['image_sha256']), 64)
        self.assertEqual(len(update['image_sha256']), 64)
        self.assertNotEqual(base['image_sha256'], update['image_sha256'])
        self.assertEqual(base['app_version'], '16')
        self.assertEqual(update['app_version'], '288')
        self.assertTrue(update['title_id'].upper().startswith('0X0005000E'))

    def test_merges_base_then_overlays_the_update(self):
        tool = load_tool()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            base = root / 'base'
            update = root / 'update'
            output = root / 'combined'
            write_dump(base, '16', {'content/base-only.txt': 'base', 'content/shared.txt': 'old'})
            write_dump(update, '288', {'content/shared.txt': 'new', 'content/update-only.txt': 'update'})

            tool.combine(base, update, output)

            self.assertEqual((output / 'code' / 'Gambit.rpx').read_text(), 'rpx 288')
            self.assertEqual((output / 'content' / 'base-only.txt').read_text(), 'base')
            self.assertEqual((output / 'content' / 'shared.txt').read_text(), 'new')
            self.assertEqual((output / 'content' / 'update-only.txt').read_text(), 'update')
            self.assertIn('288', (output / 'meta' / 'meta.xml').read_text())
            self.assertEqual((base / 'content' / 'shared.txt').read_text(), 'old')
            self.assertEqual((update / 'content' / 'shared.txt').read_text(), 'new')

    def test_refuses_to_replace_a_non_empty_output_without_force(self):
        tool = load_tool()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            base = root / 'base'
            update = root / 'update'
            output = root / 'combined'
            write_dump(base, '16', {'content/base.txt': 'base'})
            write_dump(update, '288', {'content/update.txt': 'update'})
            output.mkdir()
            (output / 'keep.txt').write_text('keep')

            with self.assertRaises(tool.CombinatorError):
                tool.combine(base, update, output)
            self.assertEqual((output / 'keep.txt').read_text(), 'keep')

            tool.combine(base, update, output, force=True)
            self.assertFalse((output / 'keep.txt').exists())
            self.assertEqual((output / 'content' / 'update.txt').read_text(), 'update')

    def test_refuses_an_output_inside_an_input(self):
        tool = load_tool()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            base = root / 'base'
            update = root / 'update'
            write_dump(base, '16', {'content/base.txt': 'base'})
            write_dump(update, '288', {'content/update.txt': 'update'})

            with self.assertRaises(tool.CombinatorError):
                tool.combine(base, update, base / 'combined')


if __name__ == '__main__':
    unittest.main()
