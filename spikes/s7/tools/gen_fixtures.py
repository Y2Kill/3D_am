"""Create exact-number JSON text fixtures; json is used only for reading/checking."""
import argparse
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
FIELD = "plugin:org.example.s7:payload"
TYPE = "plugin:org.example.s7:exact_number"
NUMBERS = {
    "control": "1", "precision": "12345678901234567890.000000000000000001",
    "uint_overflow": "18446744073709551616001", "int_overflow": "-9223372036854775809001",
    "huge_positive": "1e400", "huge_negative": "-1E-400", "exponent_style": "1E+2",
    "negative_zero": "-0", "negative_zero_float": "-0.0", "zero_exponent": "0e0",
    "trailing_zeros": "1.500", "fraction_10000": "0." + "7" * 10000,
    "exponent_1000": "1e" + "9" * 1000,
}
INVALID = ["01", "-01", "+1", ".5", "1.", "1e", "1e+", "-", "NaN", "Infinity", "-Infinity", "0x10"]


class Raw:
    def __init__(self, text):
        self.text = text


def quote(text):
    escapes = {'"': '\\"', '\\': '\\\\', '\b': '\\b', '\f': '\\f', '\n': '\\n', '\r': '\\r', '\t': '\\t'}
    return '"' + ''.join(escapes.get(c, ('\\u%04x' % ord(c)) if ord(c) < 32 else c) for c in text) + '"'


def emit(value):
    if isinstance(value, Raw):
        return value.text
    if isinstance(value, str):
        return quote(value)
    if isinstance(value, dict):
        return '{' + ','.join(quote(k) + ':' + emit(v) for k, v in value.items()) + '}'
    if isinstance(value, list):
        return '[' + ','.join(emit(v) for v in value) + ']'
    if value is None:
        return 'null'
    if isinstance(value, bool):
        return 'true' if value else 'false'
    if isinstance(value, int):
        return str(value)
    raise TypeError(type(value))


def payload(numbers):
    return {"unknown_key": {"nested": [Raw(n) for n in numbers]},
            "array": [{"another_unknown": Raw(numbers[0])}, [Raw(numbers[-1])]],
            "unknown_empty": {}, "unknown_string": "preserve \\\" café", "unknown_bool": True}


def manifest(numbers):
    doc = json.loads((ROOT / 'schemas/examples/manifest-1.0.example.json').read_text(encoding='utf-8'),
                     parse_int=Raw, parse_float=Raw)
    for namespace in ('org.example.s7one', 'org.example.s7two'):
        doc['extensions'][namespace] = {'schema_version': '1', 'data': payload(numbers)}
    doc['field_definitions'].append({'id': FIELD, 'schema_revision': 1, 'name': 'S7 plugin value',
                                    'type': TYPE, 'scopes': ['asset'],
                                    'default': payload(numbers), 'constraints': payload(numbers)})
    doc['asset']['metadata']['custom_values'].append({'field_id': FIELD, 'value': payload(numbers)})
    doc['asset']['metadata']['claims'].append({
        'id': '12345678-1234-4123-8123-123456789abc',
        'target': {'kind': 'custom', 'field_id': FIELD}, 'relation': 'alternative',
        'value': payload(numbers), 'method': 's7.fixture', 'created_at': '2026-11-10T00:00:00Z'})
    doc['files'][2]['generation']['params'] = payload(numbers)
    doc['history']['entries'][0]['details'] = payload(numbers)
    doc['history']['entries'][0]['changes'][0]['s7_extra'] = payload(numbers)
    return doc


def validate_small(directory):
    sys.path.insert(0, str(ROOT / '.cache/pylib'))
    import jsonschema
    # Exact numeric placeholders retain their lexemes and JSON number type.
    # Normative integer fields still use Python integers. No float conversion.
    validator_type = jsonschema.validators.extend(
        jsonschema.Draft202012Validator,
        type_checker=jsonschema.Draft202012Validator.TYPE_CHECKER.redefine(
            'number', lambda checker, value: isinstance(value, Raw) or
            (isinstance(value, (int, float)) and not isinstance(value, bool))))
    schema = json.loads((ROOT / 'schemas/manifest-1.0.draft.schema.json').read_text(encoding='utf-8'))
    jsonschema.Draft202012Validator.check_schema(schema)
    validator = validator_type(schema)
    count = 0
    for path in sorted(directory.glob('valid_*.json')):
        document = json.loads(path.read_text(encoding='utf-8'), parse_float=Raw)
        errors = list(validator.iter_errors(document))
        if errors:
            raise ValueError(f'SCHEMA BLOCKER {path.name}: {errors[0].json_path}: {errors[0].message}')
        count += 1
    for path in sorted(directory.glob('invalid_*.json')):
        try:
            json.loads(path.read_text(encoding='utf-8'), parse_constant=lambda s: (_ for _ in ()).throw(ValueError(s)))
        except (ValueError, json.JSONDecodeError):
            continue
        raise ValueError('Invalid number accepted: ' + path.name)
    for path in directory.glob('schema_invalid_*.json'):
        doc = json.loads(path.read_text(encoding='utf-8'), parse_float=Raw)
        if validator.is_valid(doc):
            raise ValueError('Schema-invalid document accepted: ' + path.name)
    print(f'Fixture schema: PASS ({count} valid documents, {len(INVALID)} invalid number documents)')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=pathlib.Path, required=True)
    parser.add_argument('--small', action='store_true')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    if not args.check:
        args.out.mkdir(parents=True, exist_ok=True)
        for name, token in NUMBERS.items():
            (args.out / ('valid_' + name + '.json')).write_text(emit(manifest([token])) + '\n', encoding='utf-8', newline='\n')
        (args.out / 'valid_all.json').write_text(emit(manifest(list(NUMBERS.values()))) + '\n', encoding='utf-8', newline='\n')
        for i, token in enumerate(INVALID):
            (args.out / f'invalid_{i:02}.json').write_text(emit(manifest([token])) + '\n', encoding='utf-8', newline='\n')
        for location in ('opaque', 'known'):
            doc = manifest(['1'])
            if location == 'opaque':
                doc['extensions']['org.example.s7one']['data'] = Raw('{"duplicate":1,"duplicate":2}')
            else:
                doc['asset']['metadata']['description'] = Raw('"first","description":"last"')
            (args.out / ('duplicate_' + location + '.json')).write_text(emit(doc), encoding='utf-8', newline='\n')
        doc = manifest(['1'])
        doc['asset']['metadata']['claims'][-1]['unexpected'] = True
        (args.out / 'schema_invalid_claim_extra.json').write_text(emit(doc), encoding='utf-8', newline='\n')
        if not args.small:
            for depth in (64, 1000, 100000):
                doc = manifest(['1'])
                doc['extensions']['org.example.s7one']['data'] = Raw('[' * depth + '1' + ']' * depth)
                (args.out / f'depth_{depth}.json').write_text(emit(doc), encoding='utf-8', newline='\n')
            for mib in (1, 16):
                doc = manifest(['1'])
                doc['extensions']['org.example.s7one']['data'] = Raw('0.' + '7' * (mib * 1024 * 1024 - 2))
                (args.out / f'lexeme_{mib}m.json').write_text(emit(doc), encoding='utf-8', newline='\n')
            doc = manifest(['1'])
            base = emit(doc)
            doc['extensions']['org.example.s7one']['data']['padding'] = 'z' * (64 * 1024 * 1024 - len(base.encode('utf-8')) - 13)
            (args.out / 'manifest_64m.json').write_text(emit(doc), encoding='utf-8', newline='\n')
    validate_small(args.out)


if __name__ == '__main__':
    main()
