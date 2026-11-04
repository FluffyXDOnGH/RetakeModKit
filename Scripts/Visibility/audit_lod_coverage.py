"""Read-only authoring audit of opaque LOD coverage from a native scene export.

Compares the union of coplanar opaque polygons with each LOD-0 face. This is
an authoring diagnostic, NOT a runtime visibility certificate. Moved/nonplanar
faces are reported as changed coverage, not automatically called visible holes.
No Unreal process, play session, test, or asset mutation is started here.
"""
from __future__ import annotations
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import numpy as np

AUDIT_VERSION = 1
PLANE_TOLERANCE_CM = 0.05
NORMAL_TOLERANCE = 1e-6
MAX_FRAGMENTS = 512
MAX_REPAIR_BASE_TRIANGLES = 64


def xyz(value):
    return np.asarray([value[k] for k in ('x', 'y', 'z')], dtype=float)


def signed_area(polygon):
    if len(polygon) < 3:
        return 0.0
    return float(np.sum(polygon[:, 0] * np.roll(polygon[:, 1], -1)
                        - polygon[:, 1] * np.roll(polygon[:, 0], -1)) * .5)


def clip(polygon, a, b, inside):
    """Clip a convex polygon to one exact half plane; boundaries have zero area."""
    edge = b-a
    distances = edge[0]*(polygon[:, 1]-a[1])-edge[1]*(polygon[:, 0]-a[0])
    if not inside:
        distances = -distances
    output = []
    for i, end in enumerate(polygon):
        start, ds, de = polygon[i-1], distances[i-1], distances[i]
        if (ds >= 0) != (de >= 0):
            output.append(start + (end-start) * ds/(ds-de))
        if de >= 0:
            output.append(end)
    return np.asarray(output).reshape((-1, 2))


def subtract(fragment, cover):
    """Partition fragment minus a convex cover into disjoint convex pieces."""
    remaining, pieces = fragment, []
    for a, b in zip(cover, np.roll(cover, -1, axis=0)):
        outside = clip(remaining, a, b, False)
        if abs(signed_area(outside)) > 1e-7:
            pieces.append(outside)
        remaining = clip(remaining, a, b, True)
        if len(remaining) < 3:
            break
    return pieces


def face_loss(base, lower, side):
    normal = xyz(base['normal']) * side
    vertices = np.asarray([xyz(v) for v in base['vertices']])
    origin = vertices[0]
    tangent = vertices[1]-origin
    tangent /= np.linalg.norm(tangent)
    bitangent = np.cross(normal, tangent)
    basis = np.stack([tangent, bitangent], axis=1)
    face = (vertices-origin) @ basis
    if signed_area(face) < 0:
        face = face[::-1]
    area = signed_area(face)
    uncovered, matching = [face], 0
    for candidate in lower:
        dot = float(normal @ xyz(candidate['normal']))
        if (abs(dot) if candidate['bTwoSided'] else dot) < 1-NORMAL_TOLERANCE:
            continue
        points = np.asarray([xyz(v) for v in candidate['vertices']])
        if np.max(np.abs((points-origin) @ normal)) > PLANE_TOLERANCE_CM:
            continue
        cover = (points-origin) @ basis
        if signed_area(cover) < 0:
            cover = cover[::-1]
        if signed_area(cover) < 1e-7:
            continue
        matching += 1
        uncovered = [piece for fragment in uncovered for piece in subtract(fragment, cover)]
        if len(uncovered) > MAX_FRAGMENTS:
            raise ValueError('fragment_budget')
        if not uncovered:
            break
    return area, sum(abs(signed_area(p)) for p in uncovered), matching


def audit(scene, payload):
    if scene['version'] != 3:
        raise ValueError('Expected current native schema-3 export')
    meshes = {m['componentPath']: m for m in scene['runtime_meshes']}
    rows, exclusion_sections = [], Counter()
    for component in scene['diagnostics']['components']:
        lods = component['lods']
        counts = [sum(s['triangles'] for s in lod['sections']) for lod in lods]
        row = dict(component=component['component'], mesh=component['mesh'],
                   lod_count=component['lod_count'], triangles=counts, losses=[],
                   repair_candidate=False, candidate_restrictions=[])
        rows.append(row)
        for lod in lods:
            for section in lod['sections']:
                if not section['opaque_certificate']:
                    exclusion_sections.update([('vertex_offset' if section['vertex_offset'] else
                                                'depth_offset' if section['depth_offset'] else
                                                'non_opaque_material')])
        mesh = meshes.get(component['component'])
        if not mesh:
            row['status'] = 'no_runtime_opaque_geometry'
            continue
        polygons = mesh['localPolygons']
        for p in polygons:
            values = np.asarray([xyz(v) for v in p['vertices']])
            if len(values) < 3 or not np.all(np.isfinite(values)) or not np.isclose(np.linalg.norm(xyz(p['normal'])), 1):
                raise ValueError('Invalid native polygon in '+component['component'])
        base = [p for p in polygons if p['lOD'] == 0]
        if not base:
            row['status'] = 'no_opaque_base_geometry'
            continue
        if mesh['numLODs'] == 1:
            row['status'] = 'single_lod'
            continue
        # Curved/high-detail models require a different review; keep this audit bounded.
        if len(polygons) > 512:
            row['status'] = 'manual_geometry_budget'
            continue
        try:
            for lod in range(1, mesh['numLODs']):
                lower = [p for p in polygons if p['lOD'] == lod]
                for index, face in enumerate(base):
                    for side in ((1, -1) if face['bTwoSided'] else (1,)):
                        area, lost, matching = face_loss(face, lower, side)
                        if lost > max(1.0, area*.001):
                            row['losses'].append(dict(lod=lod, base_face=index, side=side,
                                base_area_cm2=round(area, 4), uncovered_area_cm2=round(lost, 4),
                                uncovered_fraction=round(min(1., lost/area), 6),
                                coplanar_same_side_polygons=matching))
        except ValueError as error:
            row['status'] = 'manual_'+str(error)
            continue
        row['status'] = 'opaque_coverage_changed' if row['losses'] else 'opaque_coverage_preserved'
        if not row['losses']:
            continue
        restrictions = row['candidate_restrictions']
        if not re.fullmatch(r'Mesh_Brush_\d+', component['mesh'].split('.')[-1]):
            restrictions.append('not_imported_structural_brush')
        if not counts or counts[0] > MAX_REPAIR_BASE_TRIANGLES:
            restrictions.append('base_exceeds_64_triangles')
        if mesh['bDynamic']:
            restrictions.append('dynamic_geometry')
        if any(not s['opaque_certificate'] for lod in lods for s in lod['sections']):
            restrictions.append('mixed_or_non_opaque_sections')
        row['repair_candidate'] = not restrictions
    candidates = sorted({r['mesh'] for r in rows if r['repair_candidate']})
    # A shared asset is eligible only if every observed use is suitable.
    candidates = [p for p in candidates if all(r['repair_candidate'] for r in rows if r['mesh'] == p)]
    unique = {r['mesh']: r for r in rows if r['mesh'] in candidates}
    summary = dict(components=len(rows), classifications=dict(Counter(r['status'] for r in rows)),
                   candidate_assets=len(candidates), candidate_base_triangles=sum(r['triangles'][0] for r in unique.values()),
                   candidate_coarsest_triangles=sum(r['triangles'][-1] for r in unique.values()),
                   excluded_material_sections=dict(exclusion_sections))
    return dict(audit_version=AUDIT_VERSION, map=scene['map'], fingerprint=scene['fingerprint'],
                source_fingerprint=scene['source_fingerprint'], scene_sha256=hashlib.sha256(payload).hexdigest(),
                policy=dict(plane_tolerance_cm=PLANE_TOLERANCE_CM, area_tolerance_cm2=1.,
                            relative_area_tolerance=.001, candidate_max_base_triangles=MAX_REPAIR_BASE_TRIANGLES),
                limitations='Checks preservation of exported opaque planar coverage, not all visual/collision fidelity. '
                            'Changed planes can remain visually solid. Candidates need editor guards before repair. '
                            'This report never changes runtime concealment policy.',
                summary=summary, repair_candidates=candidates, components=rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('scene', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    payload = args.scene.read_bytes()
    report = audit(json.loads(payload), payload)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report['summary'], indent=2))
    print('report', args.output)


if __name__ == '__main__':
    main()
