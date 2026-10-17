"""Inventory downloaded official example ZIPs without extracting their content."""

from collections import Counter
import hashlib
import json
from pathlib import PurePosixPath
from zipfile import ZipFile

from compare_platform import ROOT


def main():
    report = {}
    for label in ("Tracks", "Rhythm"):
        path = ROOT / "research" / f"{label}.zip"
        with ZipFile(path) as archive:
            files = [entry for entry in archive.infolist() if not entry.is_dir()
                     and "__MACOSX" not in PurePosixPath(entry.filename).parts
                     and not PurePosixPath(entry.filename).name.startswith(".")]
            projects = [entry for entry in files if entry.filename.lower().endswith(".ncs")]
            banks = [entry for entry in files if entry.filename.lower().endswith(".cpb")]
            report[label.lower()] = {
                "archive_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "file_types": dict(Counter(PurePosixPath(entry.filename).suffix.lower() for entry in files)),
                "project_sizes": dict(Counter(entry.file_size for entry in projects)),
                "project_magic_counts": dict(Counter(archive.read(entry)[:4].decode("ascii") for entry in projects)),
                "patchbank_sizes": dict(Counter(entry.file_size for entry in banks)),
                "packs": sorted({"/".join(PurePosixPath(entry.filename).parts[:2]) for entry in files}),
            }
    output = ROOT / "docs/example-pack-inspection.json"
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
