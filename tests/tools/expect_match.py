"""Recursive subset matching with bijective, unordered array matching."""


def mismatch(pattern, actual, path="$", *, json_pointer=False, exact=False):
    if isinstance(pattern, dict):
        if not isinstance(actual, dict):
            return f"{path}: expected object, got {type(actual).__name__}"
        if exact:
            for key in actual.keys() - pattern.keys():
                child = path + '/' + str(key).replace('~', '~0').replace('/', '~1') if json_pointer else f"{path}.{key}"
                return f"{child}: unexpected key"
        for key, value in pattern.items():
            if key not in actual:
                child = path + '/' + str(key).replace('~', '~0').replace('/', '~1') if json_pointer else f"{path}.{key}"
                return f"{child}: missing key"
            child = path + '/' + str(key).replace('~', '~0').replace('/', '~1') if json_pointer else f"{path}.{key}"
            error = mismatch(value, actual[key], child, json_pointer=json_pointer, exact=exact)
            if error:
                return error
    elif isinstance(pattern, list):
        if not isinstance(actual, list) or len(pattern) != len(actual):
            return f"{path}: expected array length {len(pattern)}, got {actual!r}"
        edges = [[j for j, item in enumerate(actual) if mismatch(value, item, exact=exact) is None] for value in pattern]
        matched = {}

        def assign(i, seen):
            for j in edges[i]:
                if j in seen:
                    continue
                seen.add(j)
                if j not in matched or assign(matched[j], seen):
                    matched[j] = i
                    return True
            return False

        for i in range(len(pattern)):
            if not assign(i, set()):
                child = f"{path}/{i}" if json_pointer else f"{path}[{i}]"
                return f"{child}: no distinct subset match for {pattern[i]!r}; actual={actual!r}"
    elif type(pattern) is not type(actual) or pattern != actual:
        return f"{path}: expected {pattern!r}, got {actual!r}"
    return None
