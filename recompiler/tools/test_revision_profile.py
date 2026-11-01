"""Offline checks for revision selection and complete host-import relocation."""
import json,re,unittest
from pathlib import Path
from generate_revision_header import render,ROOT
class RevisionProfileTests(unittest.TestCase):
 def setUp(self):
  self.profile=json.loads((ROOT/'profiles/agmp-v288.json').read_text())
  self.mapping={int(a,16):int(b,16) for a,b in self.profile['addresses'].items()}
 def test_header_reproducible(self):
  self.assertEqual((ROOT/'recompiler/port/runtime/game_revision.h').read_text(),render(self.profile))
 def manifest_imports(self,name):
  text=(ROOT/'Manifest'/name).read_text(encoding='utf-8')
  section=text.split('\n[imports]',1)[1].split('\n[',1)[0]
  return {m.group(1):int(m.group(2),16) for m in re.finditer(r'^(\S+!\S+)\s*=\s*(0x[0-9a-fA-F]+)',section,re.M)}
 def test_manifests_agree_with_the_relocation_map(self):
  # Port code still names some imports by their v16 address (rbrew_game_address); the 288
  # manifest must put every shared import where that map sends it.
  base=self.manifest_imports('Gambit_v16.manifest');update=self.manifest_imports('Gambit_v288.manifest')
  self.assertEqual(len(base),619)
  for symbol,address in base.items():
   self.assertIn(symbol,update)
   self.assertEqual(self.mapping.get(address,address),update[symbol],symbol)
  self.assertEqual(sorted(set(update)-set(base)),['coreinit!MCP_GetTicketViews','nn_act!GetUuidEx__Q2_2nn3actFP7ACTUuidUc','sysapp!SYSLaunchSettings','sysapp!SYSSwitchToBrowser','sysapp!SYSSwitchToEShop'])
 def test_heap_function_pointer_slots_are_distinct(self):
  expected={0x02D0B8F8:0x02DD6E78,0x02D0B900:0x02DD6E80,0x02D0B908:0x02DD6E88,0x02D0B948:0x02DD6EC8}
  for old,new in expected.items():self.assertEqual(self.mapping[old],new)
  self.assertEqual(len(set(expected.values())),4)
 def test_revision_identity_is_explicit(self):
  self.assertEqual(self.profile['rpx_sha256'],'06684edefe90fea2de20430eb0e68500995980fef154caa88e4dcb59cdb7cd65')
  self.assertNotEqual(self.profile['rpx_sha256'],self.profile['base_rpx_sha256'])
  self.assertEqual(self.profile['allocator'],0x101EFCC8)
  manifest=(ROOT/'Manifest/Gambit_v288.manifest').read_text(encoding='utf-8')
  self.assertIn('image_sha256    = '+self.profile['rpx_sha256'],manifest)
if __name__=='__main__':unittest.main()
