#!/usr/bin/env python3
"""Offline, bounded 9.20/9.40 comparison. Never extracts disk SELF files."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

import generate_ps5_offsets as gen


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('dumps', type=Path)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    report, selected, assembly = {}, {}, []
    for version in ('9.20', '9.40'):
        archive = args.dumps / f'fw{version}.zip'
        with zipfile.ZipFile(archive) as z:
            bad = z.testzip()
            if bad:
                raise ValueError(f'CRC failure: {bad}')
            disk_avcap = [dict(member=i.filename, bytes=i.file_size,
                              magic=z.read(i)[:4].hex()) for i in z.infolist()
                          if i.filename.endswith('/libSceAvcap2.sprx')]
        members = gen.load_members(archive, '')
        avcaps, natives, rejected = [], [], []
        for name, data in members.items():
            try:
                layout = (gen.avcap_layout(data) if name.endswith(gen.MODULE)
                          else gen.constructor_layout(data, 0x2b0))
            except ValueError as error:
                rejected.append({'member': name, 'reason': str(error)})
                continue
            item = dict(member=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest(),
                        layout=layout, first16=data[:16].hex())
            (avcaps if name.endswith(gen.MODULE) else natives).append(item)
            selected[version, 'avcap' if name.endswith(gen.MODULE) else 'remoteplay'] = data
        if len(avcaps) != 1 or len(natives) != 1:
            raise ValueError(f'{version}: ambiguous module selection')
        merged = avcaps[0]['layout'] | natives[0]['layout']
        profile = gen.read_profile_fields(Path(__file__).resolve().parents[1] /
                    f'payload/src/platform/ps5/offset/{version.replace(".", "")}.c')
        for key, value in merged.items():
            if value != profile[key]:
                raise ValueError(f'{version}: profile mismatch {key}: {value:x} != {profile[key]:x}')
        report[version] = dict(archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
                               archive_crc='OK', avcap=avcaps, remoteplay=natives,
                               disk_avcap=disk_avcap,
                               rejected_candidates=rejected, profile_matches=True)
    # Instruction-aligned spans, chosen after manual inspection. Raw memory is
    # private; only the focused disassembly and identities are written here.
    spans = [('OpenVideo', 'avcap', 0x340, 0x690),
             ('OpenVideo IPC', 'avcap', 0x3ee0, 0x3f7a),
             ('GetVideoOutMode', 'avcap', 0x1630, 0x16a9),
             ('GetVideoOutMode IPC', 'avcap', 0x45c0, 0x4626),
             ('RemotePlay request constructor', 'remoteplay', 0x4da1d, 0x4dfae)]
    report['spans'] = []
    for name, module, start, end in spans:
        a, b = (selected[v, module][start:end] for v in ('9.20', '9.40'))
        report['spans'].append(dict(name=name, begin=hex(start), end_exclusive=hex(end),
                                    identical=a == b, sha256_940=hashlib.sha256(b).hexdigest()))
        assembly.append(f'\n{name} [{start:x},{end:x}) identical_920_940={a == b}')
        for ins in gen.disassembler().disasm(b, start):
            assembly.append(f'{ins.address:08x}: {ins.bytes.hex():24} {ins.mnemonic:10} {ins.op_str}')
    (args.out / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    (args.out / 'disassembly.txt').write_text('\n'.join(assembly) + '\n')
    print('CRC OK; both native layouts match profiles; focused spans:',
          [s['identical'] for s in report['spans']])


if __name__ == '__main__':
    main()
