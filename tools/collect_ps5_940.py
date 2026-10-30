#!/usr/bin/env python3
"""Read the one-shot fixed-file exporter on console TCP 9401. No loader call."""
import argparse
import hashlib
import json
from pathlib import Path
import socket


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('console')
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    allowed = {'open.log', 'capture.log', 'reads.csv', 'capture.h264'}
    inventory = []
    with socket.create_connection((args.console, 9401), timeout=5) as sock:
        sock.settimeout(15)
        f = sock.makefile('rb')
        if f.readline(64) != b'PBS940COLLECT1\n':
            raise ValueError('unexpected exporter protocol')
        while True:
            line = f.readline(256)
            if line == b'END\n':
                break
            command, name, size = line.decode('ascii').strip().split()
            length = int(size)
            if command != 'FILE' or name not in allowed or not -1 <= length <= 100*1024*1024:
                raise ValueError('invalid file record')
            allowed.remove(name)
            item = dict(file=name, bytes=length)
            if length >= 0:
                path = args.out / name
                digest = hashlib.sha256()
                with path.open('xb') as out:
                    left = length
                    while left:
                        block = f.read(min(left, 32768))
                        if not block:
                            raise EOFError('truncated diagnostic file')
                        out.write(block); digest.update(block); left -= len(block)
                item['sha256'] = digest.hexdigest()
            if f.read(1) != b'\n':
                raise ValueError('missing file record terminator')
            inventory.append(item)
    (args.out / 'retrieval.json').write_text(json.dumps(inventory, indent=2)+'\n')
    print(json.dumps(inventory, indent=2))


if __name__ == '__main__':
    main()
