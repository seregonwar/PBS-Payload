#!/usr/bin/env python3
"""Confirm saved FW9.40 H.264 picture throughput using ffprobe's frame count.

The elementary stream's nominal frame-rate metadata is NOT the measurement
clock. Use the monotonic read interval recorded by the diagnostic instead.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture_directory', type=Path)
    ap.add_argument('--ffprobe', default='ffprobe')
    args = ap.parse_args()
    log = (args.capture_directory / 'capture.log').read_text()
    # The directory must contain files retrieved together from ONE test run.
    runs = log.split('BEGIN kind=capture')
    if len(runs) < 2:
        ap.error('no capture run in log')
    last = runs[-1]
    match = re.search(r'capture_summary result=(-?\d+) elapsed_seconds=([\d.]+) '
                      r'reads=(\d+) bytes=(\d+) empty=(\d+) picture_starts=(\d+)', last)
    if not match or 'END result=OK cleanup=complete' not in last or int(match[1]):
        ap.error('last test failed or cleanup incomplete; inspect the log first')
    seconds = float(match[2])
    if seconds <= 0:
        ap.error('invalid elapsed measurement')
    probe = subprocess.run([args.ffprobe, '-v', 'error', '-select_streams', 'v:0',
        '-count_frames', '-show_entries', 'stream=codec_name,width,height,nb_read_frames',
        '-of', 'json', str(args.capture_directory / 'capture.h264')],
        check=True, capture_output=True, text=True)
    if probe.stderr.strip():
        ap.error('ffprobe reported decode errors: ' + probe.stderr.strip())
    streams = json.loads(probe.stdout)['streams']
    if len(streams) != 1 or streams[0].get('codec_name') != 'h264':
        ap.error('expected exactly one H.264 video stream')
    stream = streams[0]
    requested = re.search(r'fixed=(\d+)x(\d+)', last)
    expected = tuple(map(int, requested.groups())) if requested else (1920, 1080)
    if (stream.get('width'), stream.get('height')) != expected:
        ap.error(f'saved stream dimensions differ from requested {expected}')
    frames = int(stream['nb_read_frames'])
    report = dict(elapsed_seconds=seconds, decoded_frames=frames,
                  delivered_fps=frames/seconds, successful_reads=int(match[3]),
                  annexb_picture_starts=int(match[6]),
                  counts_agree=frames == int(match[6]),
                  interpretation='delivered frame throughput, not HDMI Hz or encoder maximum')
    print(json.dumps(report, indent=2))
    if not report['counts_agree']:
        raise SystemExit('Frame counts disagree; do not interpret candidate_picture_fps as validated FPS')


if __name__ == '__main__':
    main()
