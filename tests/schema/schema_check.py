"""Draft manifest 1.0 schema check (ADR-0036 item 8).

Runs the handoff cases S-01..S-17 and the earlier negative cases against
schemas/manifest-1.0.draft.schema.json with a Draft 2020-12 validator.
The schema is a DRAFT: passing these checks does not publish format 1.0.

Exit codes: 0 all cases match, 1 mismatch, 2 setup error (including a missing
jsonschema module), 77 skipped — only with --allow-skip, i.e. when Python checks
were explicitly relaxed locally (AM_ALLOW_PYTHON_SKIP=ON; CTest SKIP_RETURN_CODE).
"""
import argparse
import copy
import json
import sys

SKIP = 77

parser = argparse.ArgumentParser()
parser.add_argument("--root", required=True, help="project source directory")
parser.add_argument("--pylib", action="append", default=[], help="extra sys.path entry for jsonschema")
parser.add_argument("--allow-skip", action="store_true",
                    help="report SKIP instead of failing when jsonschema is missing (explicit local opt-out)")
args = parser.parse_args()

for p in args.pylib:
    sys.path.insert(0, p)
try:
    import jsonschema
except ImportError:
    if args.allow_skip:
        print("SKIP: Python module 'jsonschema' is not installed (AM_ALLOW_PYTHON_SKIP=ON)")
        sys.exit(SKIP)
    print("ERROR: Python module 'jsonschema' is required for schema_check "
          "(pip install jsonschema, or configure with -DAM_ALLOW_PYTHON_SKIP=ON locally)")
    sys.exit(2)

schema_path = f"{args.root}/schemas/manifest-1.0.draft.schema.json"
example_path = f"{args.root}/schemas/examples/manifest-1.0.example.json"
schema = json.load(open(schema_path, encoding="utf-8"))
example = json.load(open(example_path, encoding="utf-8"))
jsonschema.Draft202012Validator.check_schema(schema)
validator = jsonschema.Draft202012Validator(schema)


def passes(doc):
    return not any(True for _ in validator.iter_errors(doc))


MINIMAL = {
    "format": {"id": "urn:uuid:0d40b3a5-39ae-48e6-a8e8-da18b1b8c128", "version": {"major": 1, "minor": 0}},
    "asset": {"id": "550e8400-e29b-41d4-a716-446655440000", "revision": 1,
              "created_at": "2026-11-06T12:00:00Z", "modified_at": "2026-11-06T12:00:00Z"},
    "files": [],
    "history": {"entries": [{"revision": 1, "operation_id": "550e8400-e29b-41d4-a716-446655440001",
                             "timestamp": "2026-11-06T12:00:00Z", "operation": "asset_created",
                             "tool": {"application_version": "0.1.0-dev"}}]},
}
UUID = "550e8400-e29b-41d4-a716-446655440000"
AUTHOR = "28f6868e-402b-4a3a-966f-d2460d4d5d81"


def mutate(base, fn):
    doc = copy.deepcopy(base)
    fn(doc)
    return doc


def meta(doc):
    return doc["asset"].setdefault("metadata", {})


def claim(target, **extra):
    c = {"id": "e3157b5d-baa6-4a69-a082-124034ff411a", "target": target,
         "relation": "supports_selected", "method": "user_input", "created_at": "2026-11-06T18:00:00Z"}
    c.update(extra)
    return c


def field_def(type_, field_id="user:" + UUID):
    return [{"id": field_id, "schema_revision": 1, "name": "n", "type": type_, "scopes": ["asset"]}]


def ext(value):
    return {"org.example.slicer": value}


# (id, requirement, description, expected_pass, document)
CASES = [
    ("S-01", "R-09.10", "minimal manifest 1.0", True, mutate(MINIMAL, lambda d: None)),
    ("S-02", "R-09.10", "minor = 99 rejected by schema 1.0", False,
     mutate(MINIMAL, lambda d: d["format"]["version"].__setitem__("minor", 99))),
    ("S-03", "ADR-0036.7", "group without file_refs", True,
     mutate(MINIMAL, lambda d: d.__setitem__("groups", [{"id": AUTHOR, "name": "Components", "type": "part_set"}]))),
    ("S-04", "ADR-0036.7", "empty license object", False,
     mutate(MINIMAL, lambda d: meta(d).__setitem__("license", {}))),
    ("S-05", "ADR-0036.7", "link without rel", False,
     mutate(MINIMAL, lambda d: meta(d).__setitem__("links", [{"url": "https://example.com"}]))),
    ("S-06", "R-03", "authors[].id is UUIDv4", True,
     mutate(MINIMAL, lambda d: meta(d).__setitem__("authors", [{"id": AUTHOR, "name": "N"}]))),
    ("S-07", "R-02", "target standard/title", True,
     mutate(MINIMAL, lambda d: meta(d).update(title="T", claims=[claim({"kind": "standard", "field": "title"})]))),
    ("S-08", "R-02", "target custom/user:<uuid>", True,
     mutate(MINIMAL, lambda d: meta(d).update(claims=[claim({"kind": "custom", "field_id": "user:" + UUID},
                                                            relation="alternative", value="0.6")]))),
    ("S-09", "R-02", "target self", True,
     mutate(MINIMAL, lambda d: meta(d).update(claims=[claim({"kind": "self"})]))),
    ("S-10", "R-01", "legacy JSON Pointer target", False,
     mutate(MINIMAL, lambda d: meta(d).update(claims=[claim("/authors/0")]))),
    ("S-11", "R-09.1", "field type decimal", True,
     mutate(MINIMAL, lambda d: d.__setitem__("field_definitions", field_def("decimal")))),
    ("S-12", "R-09.2", "field type plugin TypeID", True,
     mutate(MINIMAL, lambda d: d.__setitem__("field_definitions",
                                             field_def("plugin:org.example.geometry:vector3",
                                                       "plugin:org.example.geometry:center_of_mass")))),
    ("S-13", "R-09.6", "field type without namespace", False,
     mutate(MINIMAL, lambda d: d.__setitem__("field_definitions", field_def("vector3")))),
    ("S-14", "R-09.9", "arbitrary JSON in extension data", True,
     mutate(MINIMAL, lambda d: d.__setitem__("extensions", ext({"schema_version": "2",
                                                               "data": {"a": [1, {"b": None, "c": "x"}]}})))),
    ("S-15", "R-09.9", "unknown key in extension envelope", False,
     mutate(MINIMAL, lambda d: d.__setitem__("extensions", ext({"data": {}, "cache": 1})))),
    ("S-16", "R-08.3", "exact duplicate tags", False,
     mutate(MINIMAL, lambda d: meta(d).__setitem__("tags", ["PETG", "PETG"]))),
    ("S-17", "R-08.3", "case-variant tags", True,
     mutate(MINIMAL, lambda d: meta(d).__setitem__("tags", ["PETG", "petg"]))),
    # Additional checks for the ADR-0036 refinements.
    ("S-18", "ADR-0036.7", "publisher object", True,
     mutate(MINIMAL, lambda d: meta(d).__setitem__("publisher", {"name": "Example Publishing",
                                                                 "url": "https://example.com"}))),
    ("S-19", "ADR-0036.7", "publisher as plain string", False,
     mutate(MINIMAL, lambda d: meta(d).__setitem__("publisher", "Example Publishing"))),
    ("S-20", "ADR-0036.7", "publisher with empty name", False,
     mutate(MINIMAL, lambda d: meta(d).__setitem__("publisher", {"name": ""}))),
    ("S-21", "ADR-0036.7", "publisher url without scheme", False,
     mutate(MINIMAL, lambda d: meta(d).__setitem__("publisher", {"name": "P", "url": "example com"}))),
    ("S-22", "R-03", "target standard_item author", True,
     mutate(MINIMAL, lambda d: meta(d).update(authors=[{"id": AUTHOR, "name": "N"}],
                                              claims=[claim({"kind": "standard_item", "field": "authors",
                                                             "item_id": AUTHOR})]))),
    ("S-23", "R-03", "standard_item on a field other than authors", False,
     mutate(MINIMAL, lambda d: meta(d).update(claims=[claim({"kind": "standard_item", "field": "tags",
                                                             "item_id": AUTHOR})]))),
    ("S-24", "R-02", "standard target with unknown field", False,
     mutate(MINIMAL, lambda d: meta(d).update(claims=[claim({"kind": "standard", "field": "colour"})]))),
    ("S-25", "ADR-0036.7", "group with empty file_refs", True,
     mutate(MINIMAL, lambda d: d.__setitem__("groups", [{"id": AUTHOR, "name": "Components", "type": "part_set",
                                                         "file_refs": []}]))),
]

# Negative cases kept from the ADR-0035 schema check (mutations of the full example).
LEGACY_NEGATIVE = {
    "size as number": lambda d: d["files"][0].__setitem__("size_bytes", 25649684),
    "size leading zero": lambda d: d["files"][0].__setitem__("size_bytes", "0123"),
    "uuid v7 id": lambda d: d["asset"].__setitem__("id", "019a5d2e-7c41-7b3e-9a10-4f2c8e1d0a77"),
    "uppercase uuid": lambda d: d["asset"].__setitem__("id", d["asset"]["id"].upper()),
    "old format id": lambda d: d["format"].__setitem__("id", "am-asset"),
    "null optional": lambda d: d["asset"]["metadata"].__setitem__("description", None),
    "unknown core key": lambda d: d.__setitem__("scenes", []),
    "traversal path": lambda d: d["files"][0].__setitem__("storage_path", "content/../x.stl"),
    "root path": lambda d: d["files"][0].__setitem__("storage_path", "x.stl"),
    "backslash path": lambda d: d["files"][0].__setitem__("storage_path", "content/a" + chr(92) + "b.stl"),
    "trailing slash": lambda d: d["files"][0].__setitem__("storage_path", "content/a/"),
    "history as array": lambda d: d.__setitem__("history", d["history"]["entries"]),
    "missing files": lambda d: d.pop("files"),
    "included w/o file_id": lambda d: d["inputs"][0]["entries"][0].pop("file_id"),
    "supports_selected with value": lambda d: d["asset"]["metadata"]["claims"][0].__setitem__("value", "x"),
    "confidence number": lambda d: d["asset"]["metadata"]["claims"][0].__setitem__("confidence", 0.9),
    "bad field id": lambda d: d["field_definitions"][0].__setitem__("id", "printed"),
    "core field id": lambda d: d["field_definitions"][0].__setitem__("id", "core:printed"),
    "revision 0": lambda d: d["asset"].__setitem__("revision", 0),
    "local tz timestamp": lambda d: d["asset"].__setitem__("created_at", "2026-11-06T12:00:00+02:00"),
}

failures = 0


def report(case_id, req, desc, expected, got):
    global failures
    ok = expected == got
    failures += not ok
    print(f"{'PASS' if ok else 'FAIL'} {case_id:<5} [{req}] expect={'valid' if expected else 'invalid'} "
          f"got={'valid' if got else 'invalid'}  {desc}")


report("EX-01", "ADR-0035", "full example manifest", True, passes(example))
for case_id, req, desc, expected, doc in CASES:
    report(case_id, req, desc, expected, passes(doc))
for i, (desc, fn) in enumerate(LEGACY_NEGATIVE.items(), 1):
    report(f"N-{i:02}", "ADR-0035", desc, False, passes(mutate(example, fn)))

total = 1 + len(CASES) + len(LEGACY_NEGATIVE)
try:
    from importlib.metadata import version as _dist_version
    _jsonschema_version = _dist_version("jsonschema")
except Exception:  # metadata may be absent for --pylib installs without dist-info
    _jsonschema_version = "unknown"
print(f"jsonschema {_jsonschema_version}; {total - failures}/{total} cases match "
      f"(S-cases: {len(CASES)}, legacy negative: {len(LEGACY_NEGATIVE)})")
sys.exit(1 if failures else 0)
