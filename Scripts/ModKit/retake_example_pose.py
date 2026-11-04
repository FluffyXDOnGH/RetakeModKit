"""Original, deterministic blockout holding/reload poses in the published mannequin reference.

No motion-library clips. Gameplay deadlines and ammunition are owned by Retake.
"""
import math


def add(a, b): return [x + y for x, y in zip(a, b)]
def sub(a, b): return [x - y for x, y in zip(a, b)]
def mul(a, s): return [x * s for x in a]
def dot(a, b): return sum(x * y for x, y in zip(a, b))
def cross(a, b): return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]
def length(a): return math.sqrt(dot(a, a))
def unit(a): return mul(a, 1.0 / max(1.e-12, length(a)))
def inverse(q): return [-q[0], -q[1], -q[2], q[3]]


def product(a, b):
    return unit([a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
                 a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
                 a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
                 a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]])


def rotate(q, v):
    q = unit(q)
    t = mul(cross(q[:3], v), 2)
    return add(v, add(mul(t, q[3]), cross(q[:3], t)))


def between(a, b):
    a, b = unit(a), unit(b)
    cosine = max(-1.0, min(1.0, dot(a, b)))
    if cosine < -.999999:
        axis = unit(cross(a, [1, 0, 0] if abs(a[0]) < .9 else [0, 0, 1]))
        return [*axis, 0.0]
    return unit([*cross(a, b), 1.0 + cosine])


def components(bones, positions, rotations):
    points, frames = [], []
    for index, bone in enumerate(bones):
        parent = bone['parent']
        if parent < 0:
            points.append(positions[index][:]); frames.append(unit(rotations[index]))
        else:
            points.append(add(points[parent], rotate(frames[parent], positions[index])))
            frames.append(product(frames[parent], rotations[index]))
    return points, frames


def reference_pose(contract):
    bones = contract['animationBones']
    return {'positions': [[b['referencePosition'][k] for k in 'xyz'] for b in bones],
            'rotations': [[b['referenceRotation'][k] for k in 'xyzw'] for b in bones],
            'scales': [[b['referenceScale'][k] for k in 'xyz'] for b in bones]}


def additive_reference_pose(contract, deltas):
    """Reconstruct a full source pose against an explicit neutral reference, preserving mesh-space deltas."""
    result = reference_pose(contract)
    bones = contract['animationBones']
    _, reference_frames = components(bones, result['positions'], result['rotations'])
    frames = [product(value['rotation'], reference_frames[i]) for i, value in enumerate(deltas)]
    for index, (bone, delta) in enumerate(zip(bones, deltas)):
        parent = bone['parent']
        result['rotations'][index] = product(inverse(frames[parent]), frames[index]) if parent >= 0 else frames[index]
        result['positions'][index] = add(result['positions'][index], delta['position'])
        result['scales'][index] = [base * (1.0 + offset) for base, offset in zip(result['scales'][index], delta['scale'])]
    return result


def arm(bones, positions, rotations, side, target, pole):
    indices = {bone['name']: index for index, bone in enumerate(bones)}
    shoulder, elbow, wrist = (indices[name + '_' + side] for name in ('upperarm', 'lowerarm', 'hand'))
    points, frames = components(bones, positions, rotations)
    start = points[shoulder]
    first, second = length(sub(points[elbow], start)), length(sub(points[wrist], points[elbow]))
    direction = unit(sub(target, start))
    distance = min(first + second - .05, max(abs(first - second) + .05, length(sub(target, start))))
    target = add(start, mul(direction, distance))
    along = (first * first - second * second + distance * distance) / (2 * distance)
    height = math.sqrt(max(0, first * first - along * along))
    pole_direction = sub(sub(pole, start), mul(direction, dot(sub(pole, start), direction)))
    desired_elbow = add(start, add(mul(direction, along), mul(unit(pole_direction), height)))
    for bone, child_index, goal in ((shoulder, elbow, desired_elbow), (elbow, wrist, target)):
        points, frames = components(bones, positions, rotations)
        delta = between(sub(points[child_index], points[bone]), sub(goal, points[bone]))
        world = product(delta, frames[bone])
        parent = bones[bone]['parent']
        rotations[bone] = product(inverse(frames[parent]), world) if parent >= 0 else world


def pose(contract, posture, reload_fraction=0.0, right_hand_rotation=None):
    bones = contract['animationBones']
    indices = {bone['name']: index for index, bone in enumerate(bones)}
    positions = [[bone['referencePosition'][key] for key in 'xyz'] for bone in bones]
    rotations = [[bone['referenceRotation'][key] for key in 'xyzw'] for bone in bones]
    scales = [[bone['referenceScale'][key] for key in 'xyz'] for bone in bones]
    upper = indices[contract['upperBodyRoot']]
    lower = []
    for index, bone in enumerate(bones):
        ancestor = index
        while ancestor >= 0 and ancestor != upper:
            ancestor = bones[ancestor]['parent']
        if ancestor < 0: lower.append(index)
    for order, index in enumerate(lower):
        value = contract['lowerBodyPoses'][posture * len(lower) + order]
        positions[index] = [value['position'][key] for key in 'xyz']
        rotations[index] = [value['rotation'][key] for key in 'xyzw']
        scales[index] = [value['scale'][key] for key in 'xyz']
    points, _ = components(bones, positions, rotations)
    chest = points[indices['spine_05']]
    right_sign = 1 if points[indices['hand_r']][0] > chest[0] else -1
    right = add(chest, [12 * right_sign, 20, -4])
    left = add(right, [0, 28.5 - 16.5 * reload_fraction, 4.5 - 6 * reload_fraction])
    arm(bones, positions, rotations, 'r', right, add(chest, [45 * right_sign, 8, -25]))
    arm(bones, positions, rotations, 'l', left, add(chest, [-45 * right_sign, 15, -23]))
    points, frames = components(bones, positions, rotations)
    hand = indices['hand_r']
    if right_hand_rotation is not None:
        rotations[hand] = product(inverse(frames[bones[hand]['parent']]), right_hand_rotation)
    points, frames = components(bones, positions, rotations)
    weapon_world = [0, 0, math.sqrt(.5), math.sqrt(.5)]  # mannequin forward is +Y; carbine geometry forward is +X
    offset_rotation = product(inverse(frames[hand]), weapon_world)
    offset_position = rotate(offset_rotation, [-3.5, 0, 6.5])  # original model's grip centre, centimetres
    return {'positions': positions, 'rotations': rotations, 'scales': scales,
            'rightHandRotation': frames[hand], 'weaponRotation': offset_rotation, 'weaponPosition': offset_position}
