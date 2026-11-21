#!/usr/bin/env python3
"""Real CDN V1.02 regression: test_cnpatch.py <evenicle2-cn-v1.02.zip> <new-work-directory>."""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
archive = pathlib.Path(sys.argv[1]).resolve(strict=True)
work = pathlib.Path(sys.argv[2]).resolve()
work.mkdir(parents=True)
libarchive = pathlib.Path(subprocess.check_output(["brew", "--prefix", "libarchive"], text=True).strip())
subprocess.run(["xcrun", "clang", "-I" + str(libarchive / "include"), "-c", str(ROOT / "ios/App/ArchiveBridge.c"),
                "-o", str(work / "bridge.o")], check=True)
sources = ["ios/App/ModArchiveExtractor.swift", "ios/App/ModCatalog.swift", "ios/App/ModOverlay.swift", "ios/Tests/CnPatchChecks.swift"]
subprocess.run(["xcrun", "swiftc", "-swift-version", "5", "-import-objc-header", str(ROOT / "ios/App/ArchiveBridge.h"),
                *[str(ROOT / source) for source in sources], str(work / "bridge.o"), "-L" + str(libarchive / "lib"),
                "-larchive", "-liconv", "-o", str(work / "checks")], check=True)
subprocess.run([str(work / "checks"), str(archive), str(work / "fixtures")], check=True)
