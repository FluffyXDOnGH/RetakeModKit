"""Explicit editable-source inventory, shared by export snapshots and recovery archives."""
from __future__ import annotations

import json
from pathlib import Path, PurePosixPath

MANIFEST = 'SourceFiles.json'
CONTENT_EXTENSIONS = {'.uasset', '.umap', '.ubulk', '.uexp', '.uptnl'}
ART_EXTENSIONS = {'.fbx', '.obj', '.mtl', '.gltf', '.glb', '.bin', '.blend', '.usd', '.usda', '.usdc',
                  '.png', '.tga', '.tif', '.tiff', '.jpg', '.jpeg', '.exr', '.hdr', '.psd', '.svg',
                  '.wav', '.flac', '.ogg', '.aiff', '.aif', '.json', '.csv', '.txt', '.md',
                  # Original model/animation/state declarations retained for an explicit source conversion workflow.
                  # Recovery preserves bytes; neither installation nor the game executes these declarations.
                  '.tik', '.skd', '.skb', '.skc', '.tan', '.st'}


def checked_path(root: Path, relative: str) -> Path:
    """Reject aliases/reparse points before resolving, including junctions on Windows."""
    if not isinstance(relative, str) or not relative or '\\' in relative or ':' in relative:
        raise ValueError(f'Invalid source path: {relative!r}')
    parts = relative.split('/')
    if relative.startswith('/') or any(not p or p in ('.', '..') or p.startswith('.') for p in parts):
        raise ValueError(f'Invalid source path: {relative!r}')
    path = root
    for part in parts:
        path = path / part
        if path.is_symlink() or getattr(path, 'is_junction', lambda: False)():
            raise ValueError(f'Source path contains a link: {relative}')
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError(f'Source path leaves the mod: {relative}')
    return path


def declared_art(value) -> list[str]:
    if not isinstance(value, dict) or set(value) != {'format', 'files'} or value['format'] != 1:
        raise ValueError('SourceFiles.json requires format 1 and an explicit files list')
    files = value['files']
    if not isinstance(files, list) or len(files) > 100000:
        raise ValueError('Invalid source-art file list')
    seen = set()
    for relative in files:
        checked_path(Path('.'), relative)
        path = PurePosixPath(relative)
        if not relative.startswith('SourceArt/') or path.suffix.lower() not in ART_EXTENSIONS:
            raise ValueError(f'Source art must be a supported data file under SourceArt/: {relative}')
        if relative.casefold() in seen:
            raise ValueError(f'Duplicate source-art path: {relative}')
        seen.add(relative.casefold())
    return sorted(files)


def inventory(plugin: Path, mod: str) -> dict[str, Path]:
    """Unreal packages plus explicitly opted-in raw art. Never archive unrelated project data."""
    plugin = Path(plugin)
    if plugin.is_symlink() or getattr(plugin, 'is_junction', lambda: False)():
        raise ValueError('The mod workspace must not be a link')
    files = {f'{mod}.uplugin': checked_path(plugin, f'{mod}.uplugin')}
    for path in (plugin / 'Content').rglob('*'):
        relative = path.relative_to(plugin).as_posix()
        checked_path(plugin, relative)
        if path.is_file() and path.suffix.lower() in CONTENT_EXTENSIONS:
            files[relative] = path
    source_list = checked_path(plugin, MANIFEST)
    if source_list.exists():
        if source_list.stat().st_size > 8 * 1024 * 1024:
            raise ValueError('SourceFiles.json exceeds 8 MiB')
        files[MANIFEST] = source_list
        for relative in declared_art(json.loads(source_list.read_text(encoding='utf-8-sig'))):
            files[relative] = checked_path(plugin, relative)
    seen = set()
    for relative, path in files.items():
        if relative.casefold() in seen or not path.is_file():
            raise ValueError(f'Missing or duplicate editable input: {relative}')
        seen.add(relative.casefold())
    return dict(sorted(files.items()))


def allowed_recovery_path(relative: str, mod: str, source_art: set[str]) -> bool:
    checked_path(Path('.'), relative)
    return (relative == f'{mod}.uplugin' or relative == MANIFEST
            or relative in source_art
            or (relative.startswith('Content/') and PurePosixPath(relative).suffix.lower() in CONTENT_EXTENSIONS))
