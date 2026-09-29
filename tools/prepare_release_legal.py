#!/usr/bin/env python3
"""Stage reproducible legal resources before signing; verify them before packaging."""
import argparse
import hashlib
import json
import plistlib
import re
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_ARCHIVE = "Bungee-7354c0c-modified-source.zip"


def write_if_changed(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


def license_text():
    text = ROOT.joinpath("EULA.md").read_text(encoding="utf-8")
    text = re.sub(r"(?m)^#{1,2}\s+", "", text).replace("**", "")
    return (text.rstrip() + "\n").encode("utf-8-sig")


def stage(output, juce, bungee, ort):
    files = {
        "THIRD_PARTY_NOTICES.txt": ROOT / "THIRD_PARTY_NOTICES.txt",
        "EULA.md": ROOT / "EULA.md",
        "PRIVACY_POLICY.md": ROOT / "PRIVACY_POLICY.md",
        "Beat-This-MIT.txt": ROOT / "licenses/Beat-This-MIT.txt",
        "Demucs-MIT.txt": ROOT / "licenses/Demucs-MIT.txt",
        "PFFFT-LICENSE.txt": ROOT / "licenses/PFFFT-LICENSE.txt",
        "Syne-OFL-1.1.txt": ROOT / "assets/Syne-OFL.txt",
        "JUCE-LICENSE.md": juce / "LICENSE.md",
        "Bungee-MPL-2.0.txt": bungee / "LICENSE",
        "ONNX-Runtime-MIT.txt": ort / "LICENSE",
        "ONNX-Runtime-ThirdPartyNotices.txt": ort / "ThirdPartyNotices.txt",
        "Cxxopts-LICENSE.txt": bungee / "submodules/cxxopts/LICENSE",
    }
    for relative in ROOT.joinpath("tools/juce_license_files.txt").read_text().splitlines():
        files["JUCE-Dependencies/" + relative] = juce / relative
    for suffix in ("MINPACK", "APACHE", "BSD", "README", "MPL2"):
        files["Eigen-COPYING." + suffix] = bungee / ("submodules/eigen/COPYING." + suffix)
    for source in files.values():
        if not source.is_file():
            raise ValueError(f"Required release notice is missing: {source}")
    for relative in ("bungee/Stream.h", "submodules/eigen/Eigen/Core", "submodules/pffft/pffft.c"):
        if not (bungee / relative).is_file():
            raise ValueError(f"Incomplete Bungee source: {relative}")
    if "void reset(" not in (bungee / "bungee/Stream.h").read_text():
        # Whitespace differs between upstream/patch revisions.
        if not re.search(r"void\s+reset\s*\(", (bungee / "bungee/Stream.h").read_text()):
            raise ValueError("Bungee source does not contain CUE's reset patch")

    output.mkdir(parents=True, exist_ok=True)
    for name, source in files.items():
        write_if_changed(output / name, source.read_bytes())
    write_if_changed(output / "LICENSE.txt", license_text())

    # Sorted entries with fixed metadata make unchanged archives byte-identical.
    temporary = output / (SOURCE_ARCHIVE + ".tmp")
    try:
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(bungee.rglob("*")):
                relative = path.relative_to(bungee)
                if ".git" in relative.parts or not path.is_file():
                    continue
                entry = zipfile.ZipInfo(relative.as_posix(), date_time=(2026, 1, 1, 0, 0, 0))
                entry.compress_type = zipfile.ZIP_DEFLATED
                entry.external_attr = (path.stat().st_mode & 0o777) << 16
                archive.writestr(entry, path.read_bytes())
        write_if_changed(output / SOURCE_ARCHIVE, temporary.read_bytes())
    finally:
        temporary.unlink(missing_ok=True)

    expected = set(files) | {"LICENSE.txt", SOURCE_ARCHIVE}
    manifest = {
        "version": project_version(),
        "platform": "macOS",
        "onnx_runtime": "1.18.1",
        "files": {name: hashlib.sha256((output / name).read_bytes()).hexdigest()
                  for name in sorted(expected)},
    }
    write_if_changed(output / "manifest.json", (json.dumps(manifest, indent=2) + "\n").encode())
    return expected | {"manifest.json"}


def project_version():
    match = re.search(r"project\(CueSampler VERSION ([0-9]+\.[0-9]+\.[0-9]+)\)",
                      ROOT.joinpath("CMakeLists.txt").read_text())
    if not match:
        raise ValueError("Cannot read CMake project version")
    return match[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--juce-source", type=Path)
    parser.add_argument("--bungee-source", type=Path)
    parser.add_argument("--ort-source", type=Path)
    parser.add_argument("--bundle", type=Path, action="append", default=[])
    parser.add_argument("--verify-bundle", type=Path, action="append", default=[])
    parser.add_argument("--license-only", action="store_true")
    args = parser.parse_args()
    write_if_changed(ROOT / "LICENSE.txt", license_text())
    if args.license_only:
        return
    deps = args.build_dir / "_deps"
    output = args.output or args.build_dir / "release-notices-mac"
    expected = stage(output, args.juce_source or deps / "juce-src",
                     args.bungee_source or deps / "bungee-src", args.ort_source or deps / "onnxruntime-src")
    for bundle in args.bundle + args.verify_bundle:
        with (bundle / "Contents/Info.plist").open("rb") as stream:
            info = plistlib.load(stream)
        if info.get("CFBundleShortVersionString") != project_version():
            raise ValueError(f"Rebuild {bundle}: binary version differs from {project_version()}")
        destination = bundle / "Contents/Resources/Licenses"
        for name in sorted(expected):
            data = (output / name).read_bytes()
            if bundle in args.bundle:
                write_if_changed(destination / name, data)
            elif not (destination / name).is_file() or (destination / name).read_bytes() != data:
                raise ValueError(f"Missing or stale signed legal resource: {destination / name}; rebuild/re-sign")
    print(f"Legal resources ready: {output}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, zipfile.BadZipFile) as error:
        sys.exit(str(error))
