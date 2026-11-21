#!/usr/bin/env python3
"""Print bounded compiler/configuration diagnostics from CI-owned build logs."""
from pathlib import Path
import re
import sys

root = Path(sys.argv[1]).resolve()
pattern = re.compile(r'error:|fatal error|CMake Error|not found|No such|failed|denied|codesign|undefined symbol', re.I)
for path in sorted(root.rglob('*.log')):
    if path.is_symlink():
        continue
    lines = path.read_text(encoding='utf-8', errors='replace').splitlines()
    selected = set()
    for i, line in enumerate(lines):
        if pattern.search(line):
            selected.update(range(max(0, i - 2), min(len(lines), i + 4)))
    if not selected:
        selected.update(range(max(0, len(lines) - 12), len(lines)))
    print('Build diagnostics:', path.relative_to(root))
    for i in sorted(selected)[-120:]:
        line = lines[i]
        if re.search(r'password|secret|token|BEGIN .*PRIVATE KEY|Developer ID Application:', line, re.I):
            line = '[credential-related diagnostic omitted]'
        line = re.sub(r'/Users/[^/\s]+', '/Users/runner', line)
        line = re.sub(r'/home/[^/\s]+', '/home/runner', line)
        print(line[:1500])
