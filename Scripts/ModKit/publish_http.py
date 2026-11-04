"""Prepare immutable static HTTP mirror files locally. This command never uploads or deploys."""
import argparse
import json
import os
import shutil
import uuid
from pathlib import Path

from retake_kit import digest, verify_export


def publish(export: Path, destination: Path, allow_candidate=False):
    export, destination = export.resolve(), destination.resolve()
    manifest = verify_export(export)
    if manifest.get('qualificationCandidate') and not allow_candidate:
        raise ValueError('Qualification candidates are internal; use --allow-candidate only for a local studio mirror')
    identity = digest(export / 'manifest.json')
    target = destination / manifest['modId'] / manifest['revision'] / identity
    if target.exists():
        if verify_export(target) != manifest:
            raise ValueError('Existing immutable mirror differs; do not overwrite it')
        return target
    incoming = destination / ('.incoming-' + uuid.uuid4().hex)
    destination.mkdir(parents=True, exist_ok=True)
    try:
        shutil.copytree(export, incoming)
        if verify_export(incoming) != manifest or digest(incoming / 'manifest.json') != identity:
            raise ValueError('Export changed during mirror staging')
        target.parent.mkdir(parents=True, exist_ok=True)
        os.rename(incoming, target)
    finally:
        if incoming.exists():
            if not incoming.resolve().is_relative_to(destination):
                raise ValueError('Unsafe staging cleanup path')
            shutil.rmtree(incoming)
    return target


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--export', required=True, type=Path)
    parser.add_argument('--destination', required=True, type=Path, help='Local web root for /retake-mods/v1')
    parser.add_argument('--allow-candidate', action='store_true')
    args = parser.parse_args()
    result = publish(args.export, args.destination, args.allow_candidate)
    print(json.dumps({'prepared': str(result), 'deployed': False, 'requires': 'HTTP byte-range responses; serve this web root read-only'}))
