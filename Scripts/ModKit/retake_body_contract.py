"""Lossless text transport for the published numeric weapon body contract.

Unreal's generic struct export text rounds numeric values; JSON readers can also lose -0 when
it is written as an integer token. The publication capture preserves signed zero explicitly.
The native body-identity check remains authoritative after import.
"""
import json
import math


def number(value):
    if type(value) not in (int, float) or not math.isfinite(value):
        raise ValueError('Body contract numbers must be finite')
    return repr(float(value))


def coordinates(value, keys):
    if set(value) != set(keys):
        raise ValueError('Unexpected body coordinate fields')
    return '(' + ','.join(key.upper() + '=' + number(value[key]) for key in keys) + ')'


def bone(value):
    return '(Name=' + json.dumps(value['name']) + ',Parent=' + str(int(value['parent'])) + ',ReferencePosition=' \
        + coordinates(value['referencePosition'], 'xyz') + ',ReferenceRotation=' + coordinates(value['referenceRotation'], 'xyzw') \
        + ',ReferenceScale=' + coordinates(value['referenceScale'], 'xyz') + ')'


def transform(value):
    return '(Position=' + coordinates(value['position'], 'xyz') + ',Rotation=' + coordinates(value['rotation'], 'xyzw') \
        + ',Scale=' + coordinates(value['scale'], 'xyz') + ')'


def import_text(value):
    return '(FormatVersion=' + str(int(value['formatVersion'])) + ',RecipeVersion=' + str(int(value['recipeVersion'])) \
        + ',UpperBodyRoot=' + json.dumps(value['upperBodyRoot']) + ',Bones=(' + ','.join(bone(x) for x in value['bones']) \
        + '),AnimationBones=(' + ','.join(bone(x) for x in value['animationBones']) + '),LowerBodyPoses=(' \
        + ','.join(transform(x) for x in value['lowerBodyPoses']) + '))'
