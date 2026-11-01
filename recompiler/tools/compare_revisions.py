"""Compare locally owned RPX revisions. Reports contain addresses only."""
import argparse, array, hashlib, json, re, struct, subprocess, sys, zlib
from pathlib import Path

class Rpx:
    def __init__(self, path):
        self.raw = Path(path).read_bytes()
        if len(self.raw) < 52 or self.raw[:6] != b'\x7fELF\x01\x02':
            raise ValueError('Expected a 32-bit big-endian Wii U ELF/RPX')
        header = struct.unpack_from('>16sHHIIIIIHHHHHH', self.raw)
        shoff, entsize, count, stridx = header[6], header[11], header[12], header[13]
        headers = [struct.unpack_from('>10I', self.raw, shoff+i*entsize) for i in range(count)]
        def data(h):
            if h[1] == 8: return b''   # SHT_NOBITS has no file content (B-08: no huge alloc)
            b = self.raw[h[4]:h[4]+h[5]]
            if h[2] & 0x08000000:
                expected = struct.unpack_from('>I', b)[0]
                b = zlib.decompress(b[4:])
                assert len(b) == expected
            return b
        names = data(headers[stridx])
        self.sections = {}
        for h in headers:
            name = names[h[0]:].split(b'\0',1)[0].decode()
            self.sections[name] = (h[3], data(h), h)
        self.start, self.text, _ = self.sections['.text']
        normalized = array.array('I')
        for (w,) in struct.iter_unpack('>I', self.text):
            op = w >> 26
            if op == 18: w &= 0xfc000003
            elif op == 16: w &= 0xffff0003
            elif op in (14,15) or 32 <= op <= 55: w &= 0xffff0000
            normalized.append(w)
        self.normalized = normalized.tobytes()
    def match(self, other, addr):
        off = addr-self.start
        if off < 0 or off+64 > len(self.text): return None
        for length in (128,96,64,48,32):
            sig = self.normalized[off:off+length]
            if len(sig) != length: continue
            pos = other.normalized.find(sig)
            if pos >= 0 and pos%4 == 0 and other.normalized.find(sig,pos+1) < 0:
                return dict(target=other.start+pos, signature_bytes=length)
        return None
    def word(self, addr):
        return struct.unpack_from('>I',self.text,addr-self.start)[0]

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--base',required=True); ap.add_argument('--update',required=True)
    ap.add_argument('--root',type=Path,required=True)
    ap.add_argument('--recompiler',type=Path,help='Optional RebrewU executable to generate named-import inventories')
    args=ap.parse_args(); root=args.root
    analysis=root/'build/analysis';analysis.mkdir(parents=True,exist_ok=True)
    if args.recompiler:
        for label,path in [('base',args.base),('update',args.update)]:
            with (analysis/f'{label}-imports.txt').open('w',encoding='utf-8') as output:
                subprocess.run([str(args.recompiler),'imports','--rpx',path],stdout=output,check=True)
    for label in ('base','update'):
        if not (analysis/f'{label}-imports.txt').exists():
            raise ValueError('Pass --recompiler to create the named-import inventories first')
    base, update = Rpx(args.base), Rpx(args.update)
    wanted = {int(l.split('#')[0].strip(),16) for l in (root/'profiles/agmp-v16.entries').read_text().splitlines() if l.strip() and not l.startswith('#')}
    wanted.update((0x02C86EA8,0x02C86B90,0x02C86CB4,0x026227C8,0x0262304C,0x025CF3A0))
    cli=(root/'recompiler/src/cli/main.cpp').read_text()
    wanted.update(int(v,16) for v in re.findall(r'0x(0[23][0-9A-Fa-f]{6})u',cli))
    matches={f'{addr:08X}':base.match(update,addr) for addr in sorted(wanted)}
    def imports(name):
        return {n:int(a,16) for n,a in re.findall(r'^(\S+)\s+stub=0x([0-9a-fA-F]+)',(root/f'build/analysis/{name}-imports.txt').read_text(),re.M)}
    old, new=imports('base'),imports('update')
    imports_map={f'{addr:08X}':new[name] for name,addr in old.items() if name in new}
    report=dict(base_sha256=hashlib.sha256(base.raw).hexdigest(),update_sha256=hashlib.sha256(update.raw).hexdigest(),
                sections={key:{n:dict(address=f'{a:08X}',size=len(b)) for n,(a,b,h) in rpx.sections.items() if h[2]&2} for key,rpx in [('base',base),('update',update)]},
                function_matches=matches, import_map=imports_map, new_imports=sorted(set(new)-set(old)),missing_imports=sorted(set(old)-set(new)))
    out=root/'build/analysis/revision-map.json';out.write_text(json.dumps(report,indent=2))
    print(f'Unique normalized signatures: {sum(v is not None for v in matches.values())}/{len(matches)}')
    print(f'Named imports mapped: {len(imports_map)}/{len(old)}; added imports: {report["new_imports"]}')
    for addr in (0x02C86EA8,0x02C86B90,0x02C86CB4):
        match=matches[f'{addr:08X}']; print(f'Allocator function {addr:08X}: {match}')
        if match:
            # Show address construction only, never original instruction bytes.
            for off in range(0,256,4):
                w=base.word(addr+off); nw=update.word(match['target']+off)
                if w>>26 in (14,15) and w&0xffff != nw&0xffff:
                    print(f'  offset {off:03X}: opcode {w>>26}, rD {(w>>21)&31}, rA {(w>>16)&31}, immediates {w&0xffff:04X}->{nw&0xffff:04X}')
    print(f'Private report: {out}')
if __name__=='__main__': main()
