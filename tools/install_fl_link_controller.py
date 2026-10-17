"""Install CUE's FL controller script without changing FL preferences or routing."""

import argparse
import ast
import datetime
import hashlib
import json
import os
from pathlib import Path
import stat
import tempfile
import uuid


SCRIPT_NAME = 'device_CueSamplerLink.py'
CONTROLLER_NAME = 'CUE SAMPLER Link'
DEFAULT_SOURCE = (Path(__file__).resolve().parent.parent
                  / 'assets/fl-studio' / CONTROLLER_NAME / SCRIPT_NAME)


def _write_atomic(destination, data, mode=0o644):
    """Stage on the destination filesystem; failed writes leave the old file intact."""
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix='.cue-link-', suffix='.tmp',
                                         dir=destination.parent, delete=False) as handle:
            temporary = Path(handle.name)
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, destination)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def install_controller(source, hardware_root, backup_root):
    """Install/update this script only; return a reviewable, JSON-safe manifest."""
    source = Path(source)
    hardware_root = Path(hardware_root).expanduser()
    backup_root = Path(backup_root).expanduser()
    data = source.read_bytes()
    text = data.decode('utf-8')
    required_headers = ('# name=' + CONTROLLER_NAME,
                        '# supportedDevices=' + CONTROLLER_NAME)
    if not all(header in text.splitlines() for header in required_headers):
        raise ValueError('Controller source has unexpected name/device metadata')
    ast.parse(text, filename=str(source))  # Validate syntax without importing FL modules.
    if backup_root.resolve().is_relative_to(hardware_root.resolve()):
        raise ValueError('Controller backups must remain outside FL Hardware scan folders')

    destination = hardware_root / CONTROLLER_NAME / SCRIPT_NAME
    if destination.is_symlink():
        raise ValueError('Refusing to replace a symlink at the controller destination')
    previous = None
    previous_mode = 0o644
    if destination.exists():
        if not destination.is_file():
            raise ValueError('Controller destination exists and is not a regular file')
        previous = destination.read_bytes()
        previous_mode = stat.S_IMODE(destination.stat().st_mode)

    digest = hashlib.sha256(data).hexdigest()
    manifest = {'status': 'unchanged', 'destination': str(destination),
                'sha256': digest, 'backup': None}
    if previous == data:
        return manifest  # Preserve inode/mtime and avoid pointless backups.

    if previous is not None:
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S_%fZ')
        backup = backup_root / (stamp + '_' + uuid.uuid4().hex) / SCRIPT_NAME
        backup.parent.mkdir(parents=True, exist_ok=False)
        _write_atomic(backup, previous, previous_mode)
        manifest['backup'] = str(backup)

    destination.parent.mkdir(parents=True, exist_ok=True)
    _write_atomic(destination, data)
    installed = destination.read_bytes()
    if installed != data or hashlib.sha256(installed).hexdigest() != digest:
        raise OSError('Installed controller verification failed')
    manifest['status'] = 'installed' if previous is None else 'updated'
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hardware-root', type=Path,
                        default=Path.home() / 'Documents/Image-Line/FL Studio/Settings/Hardware',
                        help='FL user Hardware directory; does not edit FL preferences')
    parser.add_argument('--backup-root', type=Path,
                        default=Path.home() / 'Library/Application Support/CUE SAMPLER/Controller Backups',
                        help='Backup directory outside FL Hardware scan folders')
    args = parser.parse_args(argv)
    try:
        manifest = install_controller(DEFAULT_SOURCE, args.hardware_root, args.backup_root)
    except (OSError, ValueError, SyntaxError) as error:
        parser.exit(1, 'CUE controller install failed: ' + str(error) + '\n')
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
