#!/usr/bin/env python3
"""Install a small launcher for the isolated build without editing shell profiles."""
import argparse
import os
from pathlib import Path
import shlex
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--bin-dir', type=Path, default=Path.home() / '.local/bin')
args = parser.parse_args()
binary = args.binary.resolve(strict=True)
if not binary.is_file() or not os.access(binary, os.X_OK):
    parser.error('binary must be an existing executable')
marker = '# Managed by Fincept Local CLI installer.'
destination = args.bin_dir / 'fincept-local'
if destination.exists() or destination.is_symlink():
    if destination.is_symlink() or marker not in destination.read_text():
        parser.error('destination is not our launcher; refusing to replace it')
args.bin_dir.mkdir(parents=True, exist_ok=True)
fd, temporary = tempfile.mkstemp(prefix='.fincept-local-', dir=args.bin_dir)
try:
    with os.fdopen(fd, 'w') as file:
        file.write('#!/bin/sh\n' + marker + '\nexec ' + shlex.quote(str(binary)) + ' "$@"\n')
    os.chmod(temporary, 0o755)
    os.replace(temporary, destination)
finally:
    Path(temporary).unlink(missing_ok=True)
print(destination)
