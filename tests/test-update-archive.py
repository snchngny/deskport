#!/usr/bin/env python3
"""Exercise the shipped Windows ZIP gate with disposable hostile archives."""
import pathlib
import os
import subprocess
import tempfile
import zipfile

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / "app/backend/forkupdater.cpp").read_text()
script = source.split('R"PS(', 1)[1].split(')PS"', 1)[0]
with tempfile.TemporaryDirectory(prefix="deskport-archive-") as temporary:
    base = pathlib.Path(temporary)
    cases = ["licenses/notice.txt", "../escape.txt", "folder/../../escape.txt",
             "C:/escape.txt", "folder/data:stream", "folder\\..\\..\\escape.txt", "link"]
    for index, name in enumerate(cases):
        archive, target = base / f"case-{index}.zip", base / f"out-{index}"
        with zipfile.ZipFile(archive, "w") as zipped:
            item = zipfile.ZipInfo(name)
            if name == "link":
                item.create_system = 3
                item.external_attr = 0o120777 << 16
            zipped.writestr(item, "fixture")
        result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
                                env=dict(os.environ, DESKPORT_UPDATE_ARCHIVE=str(archive)), capture_output=True, timeout=30)
        assert (result.returncode == 0) == (index == 0), (name, result.returncode, result.stderr.decode("utf-8", "replace")[-2000:])
        if index == 0:
            target.mkdir()
            subprocess.run(["tar.exe", "-xf", str(archive), "-C", str(target)], check=True, timeout=30)
            assert (target / name).read_text() == "fixture"
        else:
            assert not target.exists(), name
    assert not (base / "escape.txt").exists()
print("PASS: full Windows package extraction and six hostile ZIP rejections")
