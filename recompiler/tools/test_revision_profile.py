"""Offline checks for revision selection and the header generated from the manifests."""
import re
import unittest

from generate_revision_header import ROOT, load, render

BASE = ROOT / 'Manifest/Gambit_v16.manifest'
UPDATE = ROOT / 'Manifest/Gambit_v288.manifest'


class RevisionManifestTests(unittest.TestCase):
    def setUp(self):
        self.header = render(load(BASE), load(UPDATE))
        self.mapping = {int(a, 16): int(b, 16)
                        for a, b in re.findall(r'case (0x[0-9A-F]+)u: return (0x[0-9A-F]+)u', self.header)}

    def test_base_revision_has_no_relocation(self):
        self.assertNotIn('case 0x', render(load(BASE), load(BASE)))

    def test_every_shared_import_moves_with_its_stub(self):
        _, base, _, _ = load(BASE)
        _, update, _, _ = load(UPDATE)
        self.assertEqual(len(base), 619)
        for symbol, address in base.items():
            self.assertIn(symbol, update)
            self.assertEqual(self.mapping.get(address, address), update[symbol], symbol)

    def test_new_imports_of_the_update(self):
        _, base, _, _ = load(BASE)
        _, update, _, _ = load(UPDATE)
        self.assertEqual(sorted(set(update) - set(base)),
                         ['coreinit!MCP_GetTicketViews', 'nn_act!GetUuidEx__Q2_2nn3actFP7ACTUuidUc',
                          'sysapp!SYSLaunchSettings', 'sysapp!SYSSwitchToBrowser', 'sysapp!SYSSwitchToEShop'])

    def test_heap_function_pointer_slots_are_distinct(self):
        expected = {0x02D0B8F8: 0x02DD6E78, 0x02D0B900: 0x02DD6E80, 0x02D0B908: 0x02DD6E88, 0x02D0B948: 0x02DD6EC8}
        for old, new in expected.items():
            self.assertEqual(self.mapping[old], new)
        self.assertEqual(len(set(expected.values())), 4)

    def test_globals_become_constants(self):
        self.assertIn('RBREW_GAME_ALLOCATOR = 0x101EFCC8u', self.header)
        self.assertIn('RBREW_GAME_PLT_END = 0x02DE0000u', self.header)
        self.assertIn('#define RBREW_GAME_REVISION 288', self.header)

    def test_revision_identity_is_explicit(self):
        base = BASE.read_text(encoding='utf-8')
        update = UPDATE.read_text(encoding='utf-8')
        self.assertIn('image_sha256    = 06684edefe90fea2de20430eb0e68500995980fef154caa88e4dcb59cdb7cd65', update)
        self.assertIn('image_sha256    = 45ecd1ed81a97f46b1c8ae24820cd47fbc55399b50af09bf1f9050fe803308e5', base)

    def test_update_entries_and_hooks_live_in_the_manifest(self):
        text = UPDATE.read_text(encoding='utf-8')
        self.assertGreater(text.count('= corroborate'), 900)
        self.assertEqual(text.count('= force'), 33)
        hooks = text.split('\n[hooks]', 1)[1].split('\n[', 1)[0]
        self.assertEqual(sorted(re.findall(r'^(0x[0-9A-F]+) = hook', hooks, re.M)), ['0x0242A8F8', '0x0285C3D0'])


if __name__ == '__main__':
    unittest.main()
