"""Compare retained tagged SDK defaults without mistaking new fields for changed old fields.

Only reflected struct members added since a retained snapshot may be projected
out, including through known nested struct fields, at their native defaults.
Existing members, names, types, flags and malformed/unknown encodings stay strict.
"""
from __future__ import annotations

import re


def struct_members(value: str) -> dict[str, str] | None:
    if value == '':
        return {}
    if not isinstance(value, str) or not value.startswith('(') or not value.endswith(')'):
        return None
    text = value[1:-1]
    if not text:
        return {}
    pieces, start, depth, quoted, escaped = [], 0, 0, False, False
    for index, char in enumerate(text):
        if quoted:
            if escaped:
                escaped = False
            elif char == '\\':
                escaped = True
            elif char == '"':
                quoted = False
            continue
        if char == '"':
            quoted = True
        elif char == '(':
            depth += 1
        elif char == ')':
            depth -= 1
            if depth < 0:
                return None
        elif char == ',' and depth == 0:
            pieces.append(text[start:index])
            start = index + 1
    if quoted or escaped or depth != 0:
        return None
    pieces.append(text[start:])
    result = {}
    for piece in pieces:
        name, separator, member = piece.partition('=')
        if not separator or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', name) or name in result:
            return None
        result[name] = member
    return result


def retained_surface_changes(previous: dict[str, str], current: dict[str, str]) -> tuple[list[str], list[dict]]:
    """Return true breaks and proven reflected struct-default extensions.

    The current maintained baseline still requires exact native equality. This
    projection is solely for older readable snapshots, never arbitrary defaults.
    """
    by_cpp_type: dict[str, list[str]] = {}
    for key, value in current.items():
        if key.endswith('/kind') and value == 'struct' and previous.get(key) == 'struct':
            struct = key.removesuffix('/kind')
            by_cpp_type.setdefault('F' + struct.rsplit('.', 1)[-1], []).append(struct)
    def members_by_owner(surface):
        owners = {}
        for key in surface:
            if '/property/' in key and key.endswith('/type'):
                owner, member = key.rsplit('/property/', 1)
                owners.setdefault(owner, set()).add(member.removesuffix('/type'))
        return owners
    old_properties, new_properties = members_by_owner(previous), members_by_owner(current)
    schemas = {}
    for cpp_type, paths in by_cpp_type.items():
        if len(paths) != 1:
            continue
        owner = paths[0]
        prefix = owner + '/property/'
        extra = new_properties.get(owner, set()) - old_properties.get(owner, set())
        if all(prefix + name + '/default' in current for name in extra):
            schemas[cpp_type] = (prefix, extra)

    def compare(kind, before, after, stack=()):
        schema = schemas.get(kind)
        if schema is None or kind in stack or len(stack) >= 32:
            return before == after, []
        prefix, extra = schema
        old_members, new_members = struct_members(before), struct_members(after)
        if old_members is None or new_members is None or any(name in old_members for name in extra):
            return False, []
        # A nonzero constructor default must appear in the containing default too.
        if any(new_members.get(name, '') != current[prefix + name + '/default'] for name in extra):
            return False, []
        retained = {name: value for name, value in new_members.items() if name not in extra}
        if retained.keys() != old_members.keys():
            return False, []
        additions = sorted(name for name in extra if name in new_members)
        for name, value in old_members.items():
            member_type = current.get(prefix + name + '/type')
            # Unknown nested text remains exact; only recorded struct types allow descent.
            valid, nested = compare(member_type, value, retained[name], (*stack, kind))
            if not valid:
                return False, []
            additions.extend(name + '.' + path for path in nested)
        return True, additions

    breaks, extensions = [], []
    for key, before in previous.items():
        if key not in current:
            breaks.append(key)
            continue
        after = current[key]
        kind = current.get(key.removesuffix('/default') + '/type') if key.endswith('/default') else None
        valid, additions = compare(kind, before, after)
        if not valid:
            breaks.append(key)
        elif after != before:
            extensions.append({'path': key, 'addedMembers': sorted(additions)})
    return sorted(breaks), sorted(extensions, key=lambda r: r['path'])
