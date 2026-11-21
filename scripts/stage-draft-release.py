#!/usr/bin/env python3
"""Upload verified run outputs only to an existing private draft, never publish it."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', required=True)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--target', choices=('windows', 'macos'), required=True)
    args = parser.parse_args()
    if args.repo != 'snchngny/deskport' or not re.fullmatch(r'v[0-9][A-Za-z0-9._-]{0,79}', args.tag):
        raise SystemExit('Unexpected release destination')
    release = json.loads(subprocess.check_output(['gh', 'release', 'view', args.tag, '--repo', args.repo, '--json', 'isDraft']))
    if not release.get('isDraft'):
        raise SystemExit('Candidates may only be staged to a draft release')
    outputs = []
    for record_path in args.root.rglob('run.json'):
        record = json.loads(record_path.read_text())
        if record.get('target') != args.target or record.get('status') not in ('built', 'notarized'):
            continue
        if record.get('failed') is not False or record.get('source_unchanged') is not True:
            continue
        base = record_path.parents[3]
        dist = base / 'runs.noindex' / record['run'] / args.target / 'dist'
        for asset in record.get('assets', []):
            path = dist / asset['path']
            if not path.is_file() or path.is_symlink() or not path.resolve().is_relative_to(args.root.resolve()):
                raise SystemExit('Run asset escaped its owned build root')
            if hashlib.sha256(path.read_bytes()).hexdigest() != asset['sha256']:
                raise SystemExit('Run asset checksum mismatch')
            outputs.append(path)
    if not outputs:
        raise SystemExit('No verified packaged run found')
    subprocess.run(['gh', 'release', 'upload', args.tag, '--repo', args.repo, *map(str, outputs)], check=True)


if __name__ == '__main__':
    main()
