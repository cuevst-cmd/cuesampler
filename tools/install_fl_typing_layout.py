"""Install only CUE's optional typing layout; leave FL preferences untouched."""
import argparse
import datetime
import os
from pathlib import Path
import shutil
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--settings-dir', type=Path,
                    default=Path.home() / 'Documents/Image-Line/FL Studio/Settings')
args = parser.parse_args()
source = Path(__file__).resolve().parent.parent / 'assets/fl-studio/CUE SAMPLER Extended.scr'
data = source.read_bytes()
lines = data.decode('ascii').splitlines()
assert len(lines) == 52 and lines[42] == '72' and lines[14] == '84'
assert all(x in ('FREE', 'LOCK') or (x.isdecimal() and 72 <= int(x) <= 103) for x in lines)
destination = args.settings_dir / 'Typing to piano' / source.name
destination.parent.mkdir(parents=True, exist_ok=True)
if destination.exists() and destination.read_bytes() != data:
    backup = Path.home() / 'Library/Application Support/CUE SAMPLER/Typing Layout Backups'
    backup.mkdir(parents=True, exist_ok=True)
    shutil.copy2(destination, backup / (datetime.datetime.now().strftime('%Y%m%d_%H%M%S_%f_') + source.name))
with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as handle:
    temp_path = Path(handle.name)
    handle.write(data)
try:
    os.replace(temp_path, destination)
finally:
    temp_path.unlink(missing_ok=True)
assert destination.read_bytes() == data
print(destination)
print('Select CUE SAMPLER Extended in FL\'s Typing keyboard to piano > Layout menu.')
