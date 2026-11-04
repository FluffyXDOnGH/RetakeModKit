"""Offline conservative PVS bake. Input is RetakeVisibilityData.export_scene(), never .uasset bytes.

Uses numpy for batched whole-beam certificates. No sample-ray result can mark a cell hidden.
Unknown space is always potentially visible. Run with --help for the command-line interface.
"""
from __future__ import annotations
import argparse
import itertools
import json
import time
from collections import defaultdict
from pathlib import Path
import numpy as np


def corners(bounds):
    lo, hi = np.asarray(bounds, dtype=np.float64)
    return np.array([[hi[i] if bits[i] else lo[i] for i in range(3)]
                     for bits in itertools.product((0, 1), repeat=3)])


def merge_polygons(polygons):
    """Merge adjacent coplanar triangles only if their exact union remains convex."""
    result = [dict(p) for p in polygons]
    edges = {}
    pending = list(range(len(result)))
    while pending:
        index = pending.pop()
        polygon = result[index]
        if polygon is None:
            continue
        vertices = [tuple(v) for v in polygon['vertices']]
        normal = np.array(polygon['normal'])
        for a, b in zip(vertices, vertices[1:] + vertices[:1]):
            other_index = edges.get((b, a))
            if other_index is None or other_index == index or result[other_index] is None:
                edges[(a, b)] = index
                continue
            other = result[other_index]
            if other['two_sided'] != polygon['two_sided'] or np.dot(normal, other['normal']) < 1 - 1e-12:
                continue
            second = [tuple(v) for v in other['vertices']]
            if max(abs(np.dot(np.array(v) - vertices[0], normal)) for v in second) > 1e-6:
                continue
            pair_edges = {(x, y) for points in (vertices, second)
                          for x, y in zip(points, points[1:] + points[:1])}
            boundary = {}
            unique = True
            for points in (vertices, second):
                for x, y in zip(points, points[1:] + points[:1]):
                    if (y, x) in pair_edges:
                        continue
                    if x in boundary and boundary[x] != y:
                        unique = False
                    boundary[x] = y
            if not unique or len(boundary) < 3:
                continue
            first = next(iter(boundary))
            merged, current = [], first
            while current in boundary and current not in merged:
                merged.append(current)
                current = boundary[current]
            if current != first or len(merged) != len(boundary):
                continue
            if any(np.dot(np.cross(np.subtract(merged[(i + 1) % len(merged)], merged[i]),
                                   np.subtract(merged[(i + 2) % len(merged)], merged[(i + 1) % len(merged)])), normal) < -1e-7
                   for i in range(len(merged))):
                continue
            result[index] = dict(vertices=merged, normal=polygon['normal'], two_sided=polygon['two_sided'])
            result[other_index] = None
            pending.append(index)
            break
    return [p for p in result if p is not None]


def adaptive_cells(polygons, max_cells, min_size, bounds_override=None):
    vertices = np.concatenate([np.asarray(p['vertices']) for p in polygons])
    initial = bounds_override if bounds_override is not None else [vertices.min(axis=0) - 200, vertices.max(axis=0) + 200]
    centers = np.array([np.mean(p['vertices'], axis=0) for p in polygons])
    initial = np.array(initial, dtype=np.float64)
    indices = np.flatnonzero(np.all((centers >= initial[0]) & (centers <= initial[1]), axis=1))
    cells = [(initial, indices)]
    while len(cells) < max_cells:
        choices = [(float(np.max(b[1] - b[0])) * (1 + len(ids)), i)
                   for i, (b, ids) in enumerate(cells) if np.max(b[1] - b[0]) > min_size and len(ids) > 1]
        if not choices:
            break
        _, index = max(choices)
        bounds, ids = cells.pop(index)
        axis = int(np.argmax(bounds[1] - bounds[0]))
        cut = float(np.median(centers[ids, axis]))
        cut = max(bounds[0, axis] + (bounds[1, axis] - bounds[0, axis]) * .25,
                  min(cut, bounds[0, axis] + (bounds[1, axis] - bounds[0, axis]) * .75))
        a, b = bounds.copy(), bounds.copy()
        a[1, axis], b[0, axis] = cut, cut
        cells.extend(((a, ids[centers[ids, axis] <= cut]), (b, ids[centers[ids, axis] > cut])))
    return [b for b, _ in cells]


def certify_row(observer, targets, polygons):
    """Vectorized equivalent of RetakeVisibilityGeometry::Occludes (64 convex-hull edges)."""
    hidden = np.zeros(len(targets), dtype=bool)
    for polygon in polygons:
        vertices = np.asarray(polygon['vertices'])
        normal = np.asarray(polygon['normal'])
        da = (observer - vertices[0]) @ normal
        front = bool(da[0] > .1)
        if (not front and not polygon['two_sided']) or (front and np.any(da <= .1)) or (not front and np.any(da >= -.1)):
            continue
        indices = np.flatnonzero(~hidden)
        if not len(indices):
            break
        db = (targets[indices] - vertices[0]) @ normal
        indices = indices[np.all(db < -.1 if front else db > .1, axis=1)]
        if not len(indices):
            continue
        db = (targets[indices] - vertices[0]) @ normal
        fraction = da[None, :, None] / (da[None, :, None] - db[:, None, :])
        intersection = observer[None, :, None, :] + fraction[..., None] * (targets[indices, None, :, :] - observer[None, :, None, :])
        valid = np.ones(len(indices), dtype=bool)
        for a, b in zip(vertices, np.roll(vertices, -1, axis=0)):
            edge = b - a
            distances = np.cross(edge, intersection - a) @ normal
            valid &= np.all(distances >= .1 * np.linalg.norm(edge), axis=(1, 2))
        hidden[indices[valid]] = True
    return hidden


def runs(indices):
    output = []
    for value in indices:
        value = int(value)
        if output and output[-2] + output[-1] == value:
            output[-1] += 1
        else:
            output.extend((value, 1))
    return output


def center_ray_candidates(origin, targets, bounds):
    """A whole-beam blocker must at least intersect its center segment."""
    direction = targets - origin
    parallel = np.abs(direction) < 1e-12
    safe_direction = np.where(parallel, 1.0, direction)
    a = (bounds[0] - origin) / safe_direction
    b = (bounds[1] - origin) / safe_direction
    near = np.where(parallel, -np.inf, np.minimum(a, b))
    far = np.where(parallel, np.inf, np.maximum(a, b))
    outside = np.any(parallel & ((origin < bounds[0]) | (origin > bounds[1])), axis=1)
    return ~outside & (np.maximum(0, near.max(axis=1)) <= np.minimum(1, far.min(axis=1)))


def bake(scene, maximum_cells=256, minimum_size=400):
    if scene.get('version') != 3 or 'error' in scene or not scene.get('polygons'):
        raise ValueError('A valid native scene export with opaque polygons is required')
    groups = defaultdict(lambda: defaultdict(list))
    lod_counts = {}
    for polygon in scene['polygons']:
        normal = np.asarray(polygon['normal'], dtype=np.float64)
        vertices = np.asarray(polygon['vertices'], dtype=np.float64)
        if (normal.shape != (3,) or not np.all(np.isfinite(normal))
                or abs(float(normal @ normal) - 1.0) > 1e-5
                or vertices.ndim != 2 or vertices.shape[1] != 3 or len(vertices) < 3
                or not np.all(np.isfinite(vertices))):
            raise ValueError('Invalid world-space proof polygon: rebuild the native exporter and re-export the map')
        groups[polygon['mesh']][polygon['lod']].append(polygon)
        lod_counts[polygon['mesh']] = polygon['lod_count']
    blockers = []
    for mesh, lods in groups.items():
        # A missing/empty LOD cannot certify concealment. Never ignore that LOD.
        if len(lods) != lod_counts[mesh]:
            continue
        blockers.append([merge_polygons(lods[i]) for i in range(lod_counts[mesh])])
    polygons = [p for lods in blockers for p in lods[0]]
    if not polygons:
        raise ValueError('No opaque blockers with complete LOD coverage')
    cells = adaptive_cells(polygons, maximum_cells, minimum_size, scene.get('bake_bounds'))
    boxes = np.array([corners(b) for b in cells])
    centers = boxes.mean(axis=1)
    blocker_bounds = []
    for lods in blockers:
        vertices = np.concatenate([np.asarray(p['vertices']) for lod in lods for p in lod])
        blocker_bounds.append(np.array([vertices.min(axis=0) - .1, vertices.max(axis=0) + .1]))
    output = []
    for i, bounds in enumerate(cells):
        hidden = np.zeros(len(cells), dtype=bool)
        for lods, blocker_bound in zip(blockers, blocker_bounds):
            remaining = np.flatnonzero(~hidden)
            if not len(remaining):
                break
            remaining = remaining[center_ray_candidates(centers[i], centers[remaining], blocker_bound)]
            if not len(remaining):
                continue
            certified = np.ones(len(remaining), dtype=bool)
            for lod in lods:
                candidates = np.flatnonzero(certified)
                if not len(candidates):
                    break
                certified[candidates] = certify_row(boxes[i], boxes[remaining[candidates]], lod)
            hidden[remaining[certified]] = True
        hidden[i] = False
        output.append(dict(bounds=bounds.tolist(), hidden_runs=runs(np.flatnonzero(hidden))))
        if i % 16 == 0:
            print(f'Visibility rows {i + 1}/{len(cells)}', flush=True)
    # Neighbor portals record cell adjacency for development inspection. A connection
    # is never declared closed from samples; ambiguous portals remain open.
    portals = []
    for i, a in enumerate(cells):
        for j in range(i + 1, len(cells)):
            b = cells[j]
            if np.all(np.maximum(a[0], b[0]) <= np.minimum(a[1], b[1]) + 1e-7):
                portals.append([i, j])
    if scene.get('version') != 3 or not scene.get('source_fingerprint') or not scene.get('runtime_meshes'):
        raise ValueError('Export again with the version-3 native runtime geometry snapshot')
    return dict(version=3, map=scene['map'], fingerprint=scene['fingerprint'], cells=output,
                source_fingerprint=scene['source_fingerprint'], runtime_meshes=scene['runtime_meshes'],
                portals=portals, source_polygons=len(scene['polygons']), merged_polygons=sum(len(p) for lods in blockers for p in lods))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('scene', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--max-cells', type=int, default=256)
    parser.add_argument('--min-size-cm', type=float, default=400)
    args = parser.parse_args()
    if not 2 <= args.max_cells <= 4096 or args.min_size_cm <= 0:
        parser.error('max-cells must be 2–4096 and min-size-cm positive')
    started = time.perf_counter()
    from audit_lod_coverage import audit
    payload = args.scene.read_bytes()
    scene = json.loads(payload)
    authoring = audit(scene, payload)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    audit_path = args.output.with_suffix('.lod-audit.json')
    audit_path.write_text(json.dumps(authoring, indent=2)+'\n', encoding='utf-8')
    if authoring['repair_candidates']:
        parser.error(f"{len(authoring['repair_candidates'])} tiny opaque brush assets lose base LOD coverage. "
                     f"Review {audit_path}, repair the authored meshes, then export again. No new bake was written.")
    result = bake(scene, args.max_cells, args.min_size_cm)
    result['lod_authoring_audit'] = dict(version=authoring['audit_version'],
        scene_sha256=authoring['scene_sha256'], repair_candidates=0,
        classifications=authoring['summary']['classifications'])
    result['bake_seconds'] = time.perf_counter() - started
    args.output.write_text(json.dumps(result, separators=(',', ':')) + '\n', encoding='utf-8')
    print(json.dumps({k: result[k] for k in ('map', 'fingerprint', 'source_polygons', 'merged_polygons', 'bake_seconds')}))

if __name__ == '__main__':
    main()
