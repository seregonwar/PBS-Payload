#!/usr/bin/env python3
"""Static FW9.40 frame-rate mapping evidence; no console access or patches."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zipfile

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('archive', type=Path)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    with zipfile.ZipFile(args.archive) as z:
        def member(suffix, limit):
            entries = [i for i in z.infolist() if 'ps5-09400008-' in i.filename and
                       'pid60-' in i.filename and i.filename.endswith(suffix)]
            if len(entries) != 1 or not 0 < entries[0].file_size <= limit:
                raise ValueError('missing/ambiguous or oversized ' + suffix)
            return entries[0].filename, z.read(entries[0])
        manager_name, manager = member('/eboot.bin.prefix.bin', 8*1024*1024)
        venc_name, venc = member('/libSceVenc.sprx.prefix.bin', 4*1024*1024)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    ins = next(md.disasm(manager[0x17556:0x1755d], 0x17556))
    operand = ins.operands[1]
    if ins.mnemonic != 'lea' or operand.type != X86_OP_MEM or operand.mem.base != X86_REG_RIP:
        raise ValueError('unexpected FW9.40 frame-rate table reference')
    table = ins.address + ins.size + operand.mem.disp
    values = struct.unpack_from('<III', manager, table)
    if values != (2997, 5994, 0):
        raise ValueError('dump differs from measured FW9.40 mapping')
    report = dict(firmware='0x09400008', manager_member=manager_name,
        manager_sha256=hashlib.sha256(manager).hexdigest(), venc_member=venc_name,
        venc_sha256=hashlib.sha256(venc).hexdigest(), fps_table_rva=hex(table),
        raw_fps_table=list(values),
        fps_mode_2_is_explicit_120=False,
        observations=[
            'Manager 0x1754c loads common FPS enum; 0x17550 bounds it to 0..2.',
            'Manager 0x17556 references table; 0x17561 stores mapped value to Venc preset +0x18.',
            'Venc 0x8505..0x850d maps internal 11988 to integer 120 in another configuration path.',
            'Presence of Venc 120-rate code does not validate Avcap2 H.264 capture at 120 FPS.'],
        limits='Only this FW9.40 dump and bounded functions; no hardware limit or other-firmware conclusion.')
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out/'frame-rate-mapping.json').write_text(json.dumps(report, indent=2)+'\n')
    lines = []
    for label, data, start, end in [
        ('Manager common -> preset', manager, 0x174f0, 0x175bb),
        ('Manager normal AVC -> common conversion', manager, 0x142f0, 0x14303),
        ('Manager scan-in -> common conversion', manager, 0x138a0, 0x138ce),
        ('Venc internal rate recognition', venc, 0x8410, 0x8528),
    ]:
        lines.append('\n'+label)
        for i in md.disasm(data[start:end], start):
            lines.append(f'{i.address:08x}: {i.bytes.hex():24} {i.mnemonic:10} {i.op_str}')
    lines.append(f'\nTable @{table:x}: {values}')
    (args.out/'frame-rate-disassembly.txt').write_text('\n'.join(lines)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
