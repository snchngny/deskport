#!/usr/bin/env python3
"""Exercise response measurement with synthetic owned images and callbacks only."""
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, str(__import__('pathlib').Path(__file__).resolve().parents[1] / 'scripts'))
import build_storage
if __name__ == '__main__':
    build_storage.bootstrap()

import os
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='deskport-response-latency-') as temporary:
    work = Path(temporary)
    (work / 'test.pro').write_text(f'''QT += core gui widgets testlib
CONFIG += console c++17 testcase
CONFIG -= app_bundle
SOURCES += "{root}/tests/latency-probe.cpp" "{root}/app/gui/latencyprobe.cpp"
HEADERS += "{root}/app/gui/latencyprobe.h"
INCLUDEPATH += "{root}/app"
TARGET = latency-probe-test
''')
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen')
    subprocess.run([os.environ.get('DESKPORT_QMAKE', 'qmake'), 'test.pro'], cwd=work, env=env, check=True)
    subprocess.run(['make', '-j2'], cwd=work, env=env, check=True, stdout=subprocess.DEVNULL)
    subprocess.run([str(work / 'latency-probe-test')], cwd=work, env=env, check=True, timeout=30)
