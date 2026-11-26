"""Manifest 1.0 schema check (ADR-0036 item 8, ADR-0039).

Runs the handoff cases S-01..S-17 and the earlier negative cases against
schemas/manifest-1.0.schema.json with a Draft 2020-12 validator.
Format 1.0 is published (2026-11-14); these cases guard the published schema.

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

schema_path = f"{args.root}/schemas/manifest-1.0.schema.json"
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
     mutate(MINIMAL, lambda d: d.__setitem__("extensions", ext({"schema_version": "1", "data": {}, "cache": 1})))),
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

# ---- ADR-0037 cases (decision log v0.11 blocks, answers K-1..K-10) ----
FILE1 = "1f3403d0-93c1-4919-8ba3-ed78100b7b1f"
SHA = "a" * 64
OBJ = "60327ee9-427b-4b88-9dea-13965f61da6c"


def src_claim(target, **extra):
    return {"id": "0b6f3c8e-2a51-4d7e-9c3a-5e1f7b2d4a60", "target": target, "relation": "supports_selected",
            "method": "directory_inference", "created_at": "2026-11-06T10:15:03.120Z", **extra}


def with_source(claims):
    return lambda d: d.__setitem__("origin", {"sources": [{"id": AUTHOR, "kind": "website", "claims": claims}]})


def with_ext(**env):
    return lambda d: d.__setitem__("extensions", ext(env))


def deps(state, refs=None):
    dep = {"state": state}
    if refs is not None:
        dep["refs"] = refs
    return with_ext(schema_version="1", data={}, dependencies=dep)


def with_input(completeness="complete", obj_extra=None, entry=None, drop=None):
    def fn(d):
        o = {"id": OBJ, "kind": "archive", "name": "a.zip"}
        o.update(obj_extra or {})
        e = {"id": "e6d33bf6-4fc6-4d96-9d67-e55213718bfd", "object_id": OBJ, "entry_kind": "file",
             "path": "a.stl", "result": "excluded", "exclusion": {"rule": "r", "by": "auto"}}
        if entry is not None:
            e = entry
        inp = {"id": "debdf916-b47c-41db-b4fd-7aa9511396f3", "imported_at": "2026-11-06T10:15:03.120Z",
               "mode": "strict", "completeness": completeness, "objects": [o], "entries": [e]}
        if drop:
            inp.pop(drop)
        d["inputs"] = [inp]
    return fn


def gen_file(origin_kind, with_generation):
    f = {"id": FILE1, "storage_path": "generated/p.png", "size_bytes": "1", "sha256": SHA, "origin_kind": origin_kind}
    if with_generation:
        f["generation"] = {"tool": "core:preview.mesh", "params": {"w": 1024}}
    return lambda d: d.__setitem__("files", [f])


def fdef_c(type_, constraints, field_id="user:" + UUID):
    return lambda d: d.__setitem__("field_definitions", [{"id": field_id, "schema_revision": 1, "name": "n",
                                                          "type": type_, "scopes": ["asset"],
                                                          "constraints": constraints}])


def ext_named(name):
    return lambda d: d.__setitem__("extensions", {name: {"schema_version": "1", "data": {}}})


def history_with_range(summary):
    def fn(d):
        d["asset"]["revision"] = 4
        h = d["history"]
        last = dict(h["entries"][0], revision=4, operation="metadata_changed",
                    operation_id="550e8400-e29b-41d4-a716-446655440004")
        h["entries"].append(last)
        rng = {"from_revision": 2, "to_revision": 3}
        if summary is not None:
            rng["summary"] = summary
        h["compacted_ranges"] = [rng]
    return fn


PUB = {"name": "Example Publishing", "url": "https://example.com"}
NEW_CASES = [
    # O-01: review without state; relation/value/review/self matrix
    ("O01-1", "O-01", "alternative with review {at}", True,
     mutate(MINIMAL, lambda d: meta(d).update(title="T", claims=[claim({"kind": "standard", "field": "title"},
            relation="alternative", value="Other", review={"at": "2026-11-10T10:00:00Z"})]))),
    ("O01-2", "O-01", "review.state is no longer allowed", False,
     mutate(MINIMAL, lambda d: meta(d).update(title="T", claims=[claim({"kind": "standard", "field": "title"},
            review={"state": "confirmed", "at": "2026-11-10T10:00:00Z"})]))),
    ("O01-3", "O-01", "rejected without review", False,
     mutate(MINIMAL, lambda d: meta(d).update(claims=[claim({"kind": "standard", "field": "title"},
            relation="rejected", value="Wrong")]))),
    ("O01-4", "O-01", "rejected with value and review", True,
     mutate(MINIMAL, lambda d: meta(d).update(claims=[claim({"kind": "standard", "field": "title"},
            relation="rejected", value="Wrong", review={"at": "2026-11-10T10:00:00Z"})]))),
    ("O01-5", "O-01", "self with alternative", False,
     mutate(MINIMAL, lambda d: meta(d).update(claims=[claim({"kind": "self"}, relation="alternative", value="x")]))),
    ("O01-6", "O-01", "alternative without value", False,
     mutate(MINIMAL, lambda d: meta(d).update(claims=[claim({"kind": "standard", "field": "title"},
            relation="alternative")]))),
    # O-02: registries, standard_item, value types, sources
    ("O02-1", "O-02", "standard_item with alternative", False,
     mutate(MINIMAL, lambda d: meta(d).update(authors=[{"id": AUTHOR, "name": "N"}], claims=[claim(
         {"kind": "standard_item", "field": "authors", "item_id": AUTHOR}, relation="alternative",
         value={"name": "M"})]))),
    ("O02-2", "O-02", "nested path license.spdx as standard field", False,
     mutate(MINIMAL, lambda d: meta(d).update(claims=[claim({"kind": "standard", "field": "license.spdx"})]))),
    ("O02-3", "O-02", "source claim on standard url", True,
     mutate(MINIMAL, with_source([src_claim({"kind": "standard", "field": "url"})]))),
    ("O02-4", "O-02", "source claim on title (not in source registry)", False,
     mutate(MINIMAL, with_source([src_claim({"kind": "standard", "field": "title"})]))),
    ("O02-5", "O-02", "source claim with custom target", False,
     mutate(MINIMAL, with_source([src_claim({"kind": "custom", "field_id": "user:" + UUID})]))),
    ("O02-6", "O-02", "source claim with standard_item target", False,
     mutate(MINIMAL, with_source([src_claim({"kind": "standard_item", "field": "authors", "item_id": AUTHOR})]))),
    ("O02-7", "O-02", "alternative publisher as string", False,
     mutate(MINIMAL, lambda d: meta(d).update(publisher=PUB, claims=[claim({"kind": "standard", "field": "publisher"},
            relation="alternative", value="Other Publishing")]))),
    ("O02-8", "O-02", "alternative publisher as object", True,
     mutate(MINIMAL, lambda d: meta(d).update(publisher=PUB, claims=[claim({"kind": "standard", "field": "publisher"},
            relation="alternative", value={"name": "Other Publishing"})]))),
    ("O02-9", "O-02", "standard fields on group metadata", True,
     mutate(MINIMAL, lambda d: d.__setitem__("groups", [{"id": AUTHOR, "name": "g", "type": "variant", "file_refs": [],
                                                         "metadata": {"title": "G", "dates": {"acquired": "2025-01-31"}}}]))),
    ("O02-10", "O-02", "dates.published not YYYY-MM-DD", False,
     mutate(MINIMAL, lambda d: meta(d).__setitem__("dates", {"published": "2026-1-1"}))),
    ("O02-11", "O-02", "source url without scheme", False,
     mutate(MINIMAL, lambda d: d.__setitem__("origin", {"sources": [{"id": AUTHOR, "kind": "website",
                                                                     "url": "example.com/x"}]}))),
    # O-11: identifier grammars
    ("O11-1", "O-11", "ExtensionID a.b (minimal)", True, mutate(MINIMAL, ext_named("a.b"))),
    ("O11-2", "O-11", "ExtensionID with uppercase", False, mutate(MINIMAL, ext_named("Org.example"))),
    ("O11-3", "O-11", "ExtensionID label ending with hyphen", False, mutate(MINIMAL, ext_named("org.x-"))),
    ("O11-4", "O-11", "ExtensionID label of 64 chars", False, mutate(MINIMAL, ext_named("a." + "b" * 64))),
    ("O11-5", "O-11", "ExtensionID longer than 96 bytes", False,
     mutate(MINIMAL, ext_named(".".join(["abcdefghij"] * 9)))),
    ("O11-6", "O-11", "single-label ExtensionID", False, mutate(MINIMAL, ext_named("org"))),
    ("O11-7", "O-11/K-8", "TypeKey with trailing underscore", True,
     mutate(MINIMAL, lambda d: d.__setitem__("field_definitions", field_def("plugin:org.example:vec_",
                                                                             "plugin:org.example:com")))),
    ("O11-8", "O-11/K-8", "TypeKey ending with hyphen", False,
     mutate(MINIMAL, lambda d: d.__setitem__("field_definitions", field_def("plugin:org.example:vec-")))),
    ("O11-9", "O-11", "TypeKey with uppercase", False,
     mutate(MINIMAL, lambda d: d.__setitem__("field_definitions", field_def("plugin:org.example:Vector3")))),
    ("O11-10", "O-11", "TypeKey longer than 64 bytes", False,
     mutate(MINIMAL, lambda d: d.__setitem__("field_definitions", field_def("plugin:org.example:" + "a" * 65)))),
    ("O11-11", "O-11", "FieldKey of exactly 64 bytes", True,
     mutate(MINIMAL, lambda d: d.__setitem__("field_definitions", field_def("decimal",
                                                                             "plugin:org.example:" + "a" * 64)))),
    ("O11-12", "O-11", "TypeID with version suffix", False,
     mutate(MINIMAL, lambda d: d.__setitem__("field_definitions", field_def("plugin:org.example:vec@2")))),
    # O-12: typed dependencies
    ("O12-1", "O-12", "dependency ref without kind", False, mutate(MINIMAL, deps("declared", [{"file_id": FILE1}]))),
    ("O12-2", "O-12", "declared with empty refs", False, mutate(MINIMAL, deps("declared", []))),
    ("O12-3", "O-12", "none with refs", False, mutate(MINIMAL, deps("none", [{"kind": "file", "file_id": FILE1}]))),
    ("O12-4", "O-12", "unknown without refs", True, mutate(MINIMAL, deps("unknown"))),
    ("O12-5", "O-12", "declared file, group and field_value refs", True,
     mutate(MINIMAL, deps("declared", [{"kind": "file", "file_id": FILE1, "sha256": SHA},
                                       {"kind": "group", "group_id": AUTHOR},
                                       {"kind": "field_value", "owner_scope": "file", "owner_id": FILE1,
                                        "field_id": "user:" + UUID}]))),
    ("O12-6", "O-12", "file ref mixed with group_id", False,
     mutate(MINIMAL, deps("declared", [{"kind": "file", "file_id": FILE1, "group_id": AUTHOR}]))),
    ("O12-7", "O-12", "sha256 without file_id", False,
     mutate(MINIMAL, deps("declared", [{"kind": "file", "sha256": SHA}]))),
    ("O12-8", "O-12", "duplicate refs", False,
     mutate(MINIMAL, deps("declared", [{"kind": "group", "group_id": AUTHOR}, {"kind": "group", "group_id": AUTHOR}]))),
    ("O12-9", "O-12", "dependencies on a plugin-typed custom value", True,
     mutate(MINIMAL, lambda d: meta(d).__setitem__("custom_values", [{"field_id": "plugin:org.example.geometry:com",
            "value": [1.0, 2.5, 3], "dependencies": {"state": "declared",
                                                    "refs": [{"kind": "file", "file_id": FILE1}]}}]))),
    ("O12-10", "O-12/K-9", "unsorted refs are accepted by readers", True,
     mutate(MINIMAL, deps("declared", [{"kind": "group", "group_id": AUTHOR}, {"kind": "file", "file_id": FILE1}]))),
    # O-14: license, links.rel, generation, schema_version
    ("O14-1", "O-14", "license with empty name", False, mutate(MINIMAL, lambda d: meta(d).__setitem__("license", {"name": ""}))),
    ("O14-2", "O-14", "license url without scheme", False,
     mutate(MINIMAL, lambda d: meta(d).__setitem__("license", {"url": "example.com/license"}))),
    ("O14-3", "O-14", "license with SPDX LicenseRef", True,
     mutate(MINIMAL, lambda d: meta(d).__setitem__("license", {"spdx": "MIT OR LicenseRef-Custom"}))),
    ("O14-4", "O-14", "rel with uppercase", False,
     mutate(MINIMAL, lambda d: meta(d).__setitem__("links", [{"url": "https://x.org", "rel": "Homepage"}]))),
    ("O14-5", "O-14/K-7", "plugin rel", True,
     mutate(MINIMAL, lambda d: meta(d).__setitem__("links", [{"url": "https://x.org",
                                                              "rel": "plugin:org.example:mirror_eu"}]))),
    ("O14-6", "O-14", "unknown simple rel is kept", True,
     mutate(MINIMAL, lambda d: meta(d).__setitem__("links", [{"url": "https://x.org", "rel": "thingiverse"}]))),
    ("O14-7", "O-14", "generated file without generation", False, mutate(MINIMAL, gen_file("generated", False))),
    ("O14-8", "O-14", "imported file with generation", False, mutate(MINIMAL, gen_file("imported", True))),
    ("O14-9", "O-14", "generated file with generation", True, mutate(MINIMAL, gen_file("generated", True))),
    ("O14-10", "O-14", "extension without schema_version", False, mutate(MINIMAL, with_ext(data={}))),
    ("O14-11", "O-14", "schema_version as number", False, mutate(MINIMAL, with_ext(schema_version=2, data={}))),
    ("O14-12", "O-14", "empty schema_version", False, mutate(MINIMAL, with_ext(schema_version="", data={}))),
    # X-01: completeness, entry_kind, container, base64
    ("X01-1", "X-01/K-5", "input without completeness", False, mutate(MINIMAL, with_input(drop="completeness"))),
    ("X01-2", "X-01/K-5", "entry without entry_kind", False,
     mutate(MINIMAL, with_input(entry={"id": "e6d33bf6-4fc6-4d96-9d67-e55213718bfd", "object_id": OBJ,
                                       "path": "a.stl", "result": "not_processed"}))),
    ("X01-3", "X-01", "directory entry included without FileID", True,
     mutate(MINIMAL, with_input(entry={"id": "e6d33bf6-4fc6-4d96-9d67-e55213718bfd", "object_id": OBJ,
                                       "entry_kind": "directory", "path": "dir/", "result": "included"}))),
    ("X01-4", "X-01", "directory entry with FileID", False,
     mutate(MINIMAL, with_input(entry={"id": "e6d33bf6-4fc6-4d96-9d67-e55213718bfd", "object_id": OBJ,
                                       "entry_kind": "directory", "path": "dir/", "result": "included",
                                       "file_id": FILE1}))),
    ("X01-5", "X-01", "included file entry without FileID", False,
     mutate(MINIMAL, with_input(entry={"id": "e6d33bf6-4fc6-4d96-9d67-e55213718bfd", "object_id": OBJ,
                                       "entry_kind": "file", "path": "a.stl", "result": "included"}))),
    ("X01-6", "X-01/K-6", "container on an archive object", True,
     mutate(MINIMAL, with_input(obj_extra={"container": "rar5", "name_encoding": "utf-16(rar)"}))),
    ("X01-7", "X-01/K-6", "container on a plain file object", False,
     mutate(MINIMAL, with_input(obj_extra={"kind": "file", "container": "zip"}))),
    ("X01-8", "X-01", "strict base64 name_raw", True,
     mutate(MINIMAL, with_input(obj_extra={"name_raw": {"encoding_guess": "cp866", "base64": "YWJj"}}))),
    ("X01-9", "X-01", "base64 with bad length", False,
     mutate(MINIMAL, with_input(obj_extra={"name_raw": {"base64": "YWJ"}}))),
    ("X01-10", "X-01", "base64 with line break", False,
     mutate(MINIMAL, with_input(obj_extra={"name_raw": {"base64": "YWJj\nZGVm"}}))),
    ("X01-11", "X-01", "multivolume set with 0-based volumes", True,
     mutate(MINIMAL, with_input(obj_extra={"kind": "multivolume_archive", "container": "rar5", "volumes": [
         {"index": 0, "name": "a.part1.rar", "missing": True}, {"index": 1, "name": "a.part2.rar"}]}))),
    ("X01-12", "X-01", "unknown completeness value", False, mutate(MINIMAL, with_input(completeness="partial"))),
    # X-02: typed constraints
    ("X02-1", "X-02", "integer bounds with unit", True,
     mutate(MINIMAL, fdef_c("integer", {"minimum": {"value": "1", "unit": "mm"}, "maximum": {"value": "500", "unit": "mm"}}))),
    ("X02-2", "X-02", "integer bound with decimal value", False,
     mutate(MINIMAL, fdef_c("integer", {"minimum": {"value": "1.5"}}))),
    ("X02-3", "X-02", "decimal bound keeps trailing zeros", True,
     mutate(MINIMAL, fdef_c("decimal", {"maximum": {"value": "1.500", "unit": "mm"}}))),
    ("X02-4", "X-02", "decimal bound with exponent", False, mutate(MINIMAL, fdef_c("decimal", {"maximum": {"value": "1e3"}}))),
    ("X02-5", "X-02", "string length constraints", True, mutate(MINIMAL, fdef_c("string", {"min_length": 1, "max_length": 80}))),
    ("X02-6", "X-02", "regex constraint is not part of 1.0", False, mutate(MINIMAL, fdef_c("string", {"pattern": "^a"}))),
    ("X02-7", "X-02", "constraints on boolean", False, mutate(MINIMAL, fdef_c("boolean", {"min_length": 1}))),
    ("X02-8", "X-02", "opaque constraints on plugin type", True,
     mutate(MINIMAL, fdef_c("plugin:org.example.geometry:vector3", {"norm_max": 12345678901234567890.5},
                            "plugin:org.example.geometry:com"))),
    ("X02-9", "X-02", "multi_enum item constraints", True, mutate(MINIMAL, fdef_c("multi_enum", {"min_items": 1, "max_items": 3}))),
    ("X02-10", "X-02", "date bounds", True,
     mutate(MINIMAL, fdef_c("date", {"minimum": {"value": "2000-01-01"}}))),
    ("X02-11", "X-02", "empty constraints object", False, mutate(MINIMAL, fdef_c("integer", {}))),
    # O-18: compacted ranges
    ("O18-1", "O-18", "compacted range with typed summary", True,
     mutate(MINIMAL, history_with_range({"revision_count": 2, "operation_counts": {"metadata_changed": 2}}))),
    ("O18-2", "O-18", "compacted range without summary", False, mutate(MINIMAL, history_with_range(None))),
    ("O18-3", "O-18", "summary with revision_count 0", False, mutate(MINIMAL, history_with_range({"revision_count": 0}))),
    ("O18-4", "O-18", "operation_counts with unknown operation", False,
     mutate(MINIMAL, history_with_range({"revision_count": 2, "operation_counts": {"made_up": 2}}))),
    ("O18-5", "O-18", "free-form summary key", False,
     mutate(MINIMAL, history_with_range({"revision_count": 2, "note": "x"}))),
]
CASES.extend(NEW_CASES)


# ADR-0039 (report 08): sub-details of X-01, X-02, O-14, O-15, O-18 closed.
def fdef_x(type_, **extra):
    d = {"id": "user:" + UUID, "schema_revision": 1, "name": "n", "type": type_, "scopes": ["asset"]}
    d.update(extra)
    return lambda doc: doc.__setitem__("field_definitions", [d])


def set_operation(name):
    return lambda d: d["history"]["entries"][0].__setitem__("operation", name)


def set_change_kind(kind):
    return lambda d: d["history"]["entries"][0].__setitem__("changes", [{"kind": kind}])


def file_type(type_id):
    f = {"id": FILE1, "storage_path": "content/" + UUID + "/a.stl", "size_bytes": "1", "sha256": SHA,
         "origin_kind": "user_added", "type": {"id": type_id, "by": "signature"}}
    return lambda d: d.__setitem__("files", [f])


def gen_tool(tool):
    f = {"id": FILE1, "storage_path": "generated/p.png", "size_bytes": "1", "sha256": SHA,
         "origin_kind": "generated", "generation": {"tool": tool}}
    return lambda d: d.__setitem__("files", [f])


PLUGIN_KEY = "plugin:org.example.slicer:density"
ADR0039_CASES = [
    ("F1-1", "ADR-0039.1", "base dimension key", True, mutate(MINIMAL, fdef_x("decimal", dimension="length"))),
    ("F1-2", "ADR-0039.1", "unknown but valid dimension key", True, mutate(MINIMAL, fdef_x("decimal", dimension="density"))),
    ("F1-3", "ADR-0039.1", "plugin dimension key", True, mutate(MINIMAL, fdef_x("decimal", dimension=PLUGIN_KEY))),
    ("F1-4", "ADR-0039.1", "dimension with upper case", False, mutate(MINIMAL, fdef_x("decimal", dimension="Length"))),
    ("F1-5", "ADR-0039.1", "dimension with space", False, mutate(MINIMAL, fdef_x("decimal", dimension="linear size"))),
    ("F2-1", "ADR-0039.2", "decimal default {value, unit}", True,
     mutate(MINIMAL, fdef_x("decimal", dimension="length", default={"value": "1.5", "unit": "mm"}))),
    ("F2-2", "ADR-0039.2", "decimal default plain string", True, mutate(MINIMAL, fdef_x("decimal", default="1.5"))),
    ("F2-3", "ADR-0039.2", "decimal default object without unit", False,
     mutate(MINIMAL, fdef_x("decimal", default={"value": "1.5"}))),
    ("F2-4", "ADR-0039.2", "integer default object with extra key", False,
     mutate(MINIMAL, fdef_x("integer", default={"value": "2", "unit": "mm", "note": "x"}))),
    ("F2-5", "ADR-0039.2", "integer default with decimal value", False,
     mutate(MINIMAL, fdef_x("integer", default={"value": "2.5", "unit": "mm"}))),
    ("F4-1", "ADR-0039.4", "claim with unit", True,
     mutate(MINIMAL, lambda d: meta(d).__setitem__("claims", [claim({"kind": "custom", "field_id": "user:" + UUID},
                                                                     relation="alternative", value="12.5", unit="mm")]))),
    ("F4-2", "ADR-0039.4", "claim with empty unit", False,
     mutate(MINIMAL, lambda d: meta(d).__setitem__("claims", [claim({"kind": "custom", "field_id": "user:" + UUID},
                                                                     relation="alternative", value="12.5", unit="")]))),
    ("F5-1", "ADR-0039.5", "base change kind", True, mutate(MINIMAL, set_change_kind("file_add"))),
    ("F5-2", "ADR-0039.5", "unknown valid change kind is preserved", True, mutate(MINIMAL, set_change_kind("preview_set_primary"))),
    ("F5-3", "ADR-0039.5", "plugin change kind", True, mutate(MINIMAL, set_change_kind(PLUGIN_KEY))),
    ("F5-4", "ADR-0039.5", "change kind with upper case", False, mutate(MINIMAL, set_change_kind("FileAdd"))),
    ("F6-1", "ADR-0039.6", "plugin operation", True, mutate(MINIMAL, set_operation(PLUGIN_KEY))),
    ("F6-2", "ADR-0039.6", "operation with space", False, mutate(MINIMAL, set_operation("files changed"))),
    ("F8-1", "ADR-0039.8", "generation.tool core:", True, mutate(MINIMAL, gen_tool("core:preview.mesh"))),
    ("F8-2", "ADR-0039.8", "generation.tool plugin:", True, mutate(MINIMAL, gen_tool("plugin:org.example.slicer:gcode"))),
    ("F8-3", "ADR-0039.8", "generation.tool without namespace", False, mutate(MINIMAL, gen_tool("am.preview.mesh"))),
    ("F8-4", "ADR-0039.8", "generation.tool empty core key", False, mutate(MINIMAL, gen_tool("core:"))),
    ("F10-1", "ADR-0039.10", "files[].type.id dotted", True, mutate(MINIMAL, file_type("mesh.stl.binary"))),
    ("F10-2", "ADR-0039.10", "files[].type.id with upper case", False, mutate(MINIMAL, file_type("Mesh.STL"))),
]
CASES.extend(ADR0039_CASES)

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
from pathlib import Path

expected_schema = json.loads((Path(args.root) / "schemas/expected.v1.schema.json").read_text(encoding="utf-8"))
jsonschema.Draft202012Validator.check_schema(expected_schema)
expected_validator = jsonschema.Draft202012Validator(expected_schema)
expected_files = sorted((Path(args.root) / "tests/fixtures/golden").glob("*/expected.json"))
if not expected_files:
    print("FAIL expected schema: no golden cases found")
    failures += 1
for path in expected_files:
    errors = sorted(expected_validator.iter_errors(json.loads(path.read_text(encoding="utf-8"))), key=lambda e: str(e.path))
    for error in errors:
        print(f"FAIL expected schema: {path.parent.name}: {error.json_path}: {error.message}")
    failures += len(errors)
print(f"expected schema: {len(expected_files)} golden expectations checked")
# T-027 diagnostics are independently versioned; the published manifest stays unchanged.
log_example = {"v": 1, "ts": "2026-11-23T10:15:30.123Z", "level": "info", "component": "test",
               "event": "test.event", "msg": "Example", "pid": 1, "tid": 2,
               "ctx": dict.fromkeys(("job_id", "asset_id", "library_id", "operation")),
               "fields": {"maximum": 18446744073709551615}, "kinds": {}}
diag_example = {"format": "am-diagnostics", "version": 1, "created_at": "2026-11-23T10:15:30.123Z",
                "app": {"version": "0.1.0", "build": "C++20"},
                "system": {"os": "test", "os_version": "1", "arch": "test", "cpu_count": 1},
                "logs": [], "redaction": {"salted": True, "rules": ["path_hmac_sha256"]}}
for name, positive, fixture_dir in (("log-record", log_example, ".cache/log-record-tests"),
                                     ("diagnostics", diag_example, ".cache/diag-export-cases")):
    current_schema = json.loads((Path(args.root) / f"schemas/{name}.v1.schema.json").read_text("utf8"))
    jsonschema.Draft202012Validator.check_schema(current_schema)
    current = jsonschema.Draft202012Validator(current_schema, format_checker=jsonschema.FormatChecker())
    cases = [("positive", positive, True), ("extra", dict(positive, unexpected=True), False)]
    if name == "log-record":
        missing = dict(positive)
        del missing["ctx"]
        cases += [("level", dict(positive, level="invalid"), False), ("ctx", missing, False)]
    else:
        cases += [("format", dict(positive, format="invalid"), False)]
        unavailable = {"source_kind": "log", "name": "am-20261124T000000Z-123.jsonl", "reason": "cloud_placeholder"}
        cases += [("unavailable", dict(positive, unavailable=[unavailable]), True),
                  ("unavailable-database", dict(positive, unavailable=[dict(unavailable, source_kind="database", name="index.sqlite")]), True),
                  ("unavailable-library", dict(positive, unavailable=[dict(unavailable, source_kind="library", name="am-library.toml")]), True),
                  ("unavailable-extra", dict(positive, unavailable=[dict(unavailable, extra=True)]), False),
                  ("unavailable-reason", dict(positive, unavailable=[dict(unavailable, reason="unknown")]), False),
                  ("unavailable-kind", dict(positive, unavailable=[dict(unavailable, source_kind="unknown")]), False),
                  ("unavailable-posix-path", dict(positive, unavailable=[dict(unavailable, name="/home/user/log.jsonl")]), False),
                  ("unavailable-windows-path", dict(positive, unavailable=[dict(unavailable, name="C:\\Users\\user\\log.jsonl")]), False),
                  ("unavailable-empty", dict(positive, unavailable=[]), False)]
    for label, value, expected in cases:
        report(name + ":" + label, "NFR-14.4.9", "diagnostic schema", expected, current.is_valid(value))
    actual = list((Path(args.root) / fixture_dir).glob("*.json"))
    for path in actual:
        report(name + ":" + path.stem, "NFR-14.4.9", "actual serialized record", True,
               current.is_valid(json.loads(path.read_text("utf8"))))
    print(f"{name} schema: {len(cases)} contract cases and {len(actual)} actual records checked")
import_plan_validators = {}
for version in (1, 2, 3):
    current = json.loads((Path(args.root) / f"schemas/import-plan.v{version}.schema.json").read_text("utf8"))
    jsonschema.Draft202012Validator.check_schema(current)
    import_plan_validators[version] = jsonschema.Draft202012Validator(current)
import_example = {"format": "am-import-plan", "version": 1, "created_at": "2026-11-23T00:00:00.000Z",
                  "generator": {"name": "am", "version": "test"},
                  "profile": {"id": "default", "version": 1, "sha256": "0" * 64, "not_applied": []},
                  "mode": "strict", "roots": [{"path": "/synthetic"}],
                  "scan": {"completeness": "complete", "limits_hit": [], "counts": {"nodes": 0, "files": 0, "unavailable": 0}},
                  "nodes": [], "candidates": []}
plan_contract_counts = {1: 0, 2: 0, 3: 0}
for label, value, expected in (("positive", import_example, True),
                               ("version", dict(import_example, version=2), False),
                               ("extra", dict(import_example, extra=True), False),
                               ("missing", {key: value for key, value in import_example.items() if key != "roots"}, False)):
    report("import-plan:v1:" + label, "SC-1", "import plan schema", expected, import_plan_validators[1].is_valid(value))
    plan_contract_counts[1] += 1

# Static nonempty v1 cases survive replacement of the scan-generated cache by v2.
def source_candidate():
    return {"id": "c1", "kind": "new_asset", "root": 0, "base_path": "", "title_hint": "Synthetic",
            "context": {}, "confidence": "high", "reasons": ["root_file"],
            "inputs": [{"id": "i1", "kind": "file", "objects": [{"node": "n1"}]}],
            "entries": [{"node": "n1", "decision": "include"}], "directories": [], "volume_sets": [],
            "suggested_links": [], "matches": [], "warnings": [], "action": "import"}

v1_nonempty = copy.deepcopy(import_example)
v1_nonempty["nodes"] = [{"id": "n1", "root": 0, "path": "source", "name": "source", "kind": "file",
                          "status": "available", "size": "3", "mtime_ns": "123", "detected": "unknown"}]
v1_nonempty["candidates"] = [source_candidate()]
v1_cases = [("file", copy.deepcopy(v1_nonempty))]
for kind in ("directory", "archive", "multivolume_archive"):
    sample = copy.deepcopy(v1_nonempty)
    sample["candidates"][0]["inputs"][0]["kind"] = kind
    if kind == "directory":
        sample["nodes"][0]["kind"] = "directory"
        sample["candidates"][0]["directories"] = ["n1"]
        sample["candidates"][0]["entries"] = []
    if kind == "multivolume_archive":
        sample["candidates"][0]["volume_sets"] = [{"id": "v1", "scheme": "rar_part", "base": "source",
            "members": ["n1"], "missing_indices": [], "confirmed": False}]
        sample["candidates"][0]["inputs"][0]["objects"] = [{"volume_set": "v1"}]
    v1_cases.append((kind, sample))
sample = copy.deepcopy(v1_nonempty)
sample["candidates"][0]["kind"] = "ready_asset"
sample["nodes"][0]["detected"] = "archive.zip"
sample["candidates"][0]["reasons"] = ["ready_asset_invalid"]
sample["candidates"][0]["action"] = "ask"
v1_cases.append(("ready", sample))
sample = copy.deepcopy(v1_nonempty)
sample["nodes"][0].update(status="unavailable", problem={"code": "cloud_placeholder", "message_id": "am.import.cloud_placeholder"},
                           name_raw="ffff", path_raw="ffff", raw_encoding="utf-16le")
sample["candidates"][0]["entries"][0]["decision"] = "ask"
v1_cases.append(("raw-unavailable", sample))
sample = copy.deepcopy(v1_nonempty)
sample["nodes"][0]["kind"] = "symlink"
sample["candidates"][0]["entries"][0]["decision"] = "skip"
v1_cases.append(("link", sample))
for label, value in v1_cases:
    report("import-plan:v1:" + label, "SC-1", "static v1 plan", True, import_plan_validators[1].is_valid(value))
    plan_contract_counts[1] += 1

v2_example = copy.deepcopy(v1_nonempty)
v2_example["version"] = 2
v2_example["profile"]["limits"] = {"max_entries": "100000", "max_unpacked_bytes": "214748364800", "max_ratio": 200,
                                    "max_seconds_per_container": "3600", "max_temp_bytes": "107374182400"}
v2_example["candidates"][0]["inputs"][0]["kind"] = "archive"
v2_example["archives"] = [{"node": "n1", "container": "zip", "status": "listed", "entry_count": 1,
                            "name_encodings": ["utf-8(valid)"], "zip64": False, "comment_length": 0,
                            "encrypted_entries": 0, "unsupported_entries": 0}]
v2_example["archive_entries"] = [{"id": "a1", "archive": "n1", "index": 0, "path": "part.bin", "path_raw": "706172742e62696e",
    "name_encoding": "utf-8(valid)", "encoding_exact": True, "kind": "file", "size": "3", "compressed_size": "3",
    "crc32": "00000000", "method": 0, "encrypted": False, "detected": "unknown", "status": "available"}]
v2_example["candidates"][0]["entries"].append({"archive_entry": "a1", "decision": "include"})
v2_cases = [("positive", copy.deepcopy(v2_example), True)]
for label, section, field, value in (("entry-kind", "archive_entries", "kind", "device"),
                                      ("entry-extra", "archive_entries", "extra", True),
                                      ("entry-hex", "archive_entries", "path_raw", "AB")):
    sample = copy.deepcopy(v2_example)
    sample[section][0][field] = value
    v2_cases.append((label, sample, False))
sample = copy.deepcopy(v2_example)
sample["candidates"][0]["entries"][0]["archive_entry"] = "a1"
v2_cases.append(("both-references", sample, False))
sample = copy.deepcopy(v2_example)
del sample["profile"]["limits"]
v2_cases.append(("missing-limits", sample, False))
sample = copy.deepcopy(v2_example)
sample["profile"]["limits"]["extra"] = "1"
v2_cases.append(("extra-limit", sample, False))
sample = copy.deepcopy(v2_example)
sample["profile"]["limits"]["max_entries"] = "0"
v2_cases.append(("zero-limit", sample, False))
for label, value, expected in v2_cases:
    report("import-plan:v2:" + label, "SC-1", "static v2 plan", expected, import_plan_validators[2].is_valid(value))
    plan_contract_counts[2] += 1

# T-032: v3 adds nested archives keyed by their parent record, depth, digest, access and expand.
v3_example = copy.deepcopy(v2_example)
v3_example["version"] = 3
v3_example["profile"]["limits"]["max_depth"] = "3"
v3_example["archives"][0]["depth"] = 1
v3_example["archive_entries"][0]["path"] = "inner.zip"
v3_example["archive_entries"][0]["path_raw"] = "696e6e65722e7a6970"
v3_example["archive_entries"][0]["detected"] = "archive.zip"
v3_example["archives"].append({"entry": "a1", "depth": 2, "sha256": "0" * 64, "access": "range", "container": "zip",
                               "status": "listed", "entry_count": 1, "name_encodings": ["utf-8(valid)"], "zip64": False,
                               "comment_length": 0, "encrypted_entries": 0, "unsupported_entries": 0})
nested_record = copy.deepcopy(v2_example["archive_entries"][0])
nested_record["id"] = "a2"
nested_record["archive"] = "a1"
v3_example["archive_entries"].append(nested_record)
v3_example["candidates"][0]["entries"][-1]["decision"] = "expand"
v3_example["candidates"][0]["entries"].append({"archive_entry": "a2", "decision": "include"})
v3_cases = [("positive", copy.deepcopy(v3_example), True)]
sample = copy.deepcopy(v3_example)
del sample["profile"]["limits"]["max_depth"]
v3_cases.append(("missing-max-depth", sample, False))
sample = copy.deepcopy(v3_example)
del sample["archives"][1]["depth"]
v3_cases.append(("missing-depth", sample, False))
sample = copy.deepcopy(v3_example)
sample["archives"][1]["node"] = "n1"
v3_cases.append(("node-and-entry", sample, False))
sample = copy.deepcopy(v3_example)
sample["archives"][0]["access"] = "temp"
v3_cases.append(("top-level-access", sample, False))
sample = copy.deepcopy(v3_example)
sample["archives"][1]["access"] = "disk"
v3_cases.append(("unknown-access", sample, False))
sample = copy.deepcopy(v2_example)
sample["candidates"][0]["entries"][-1]["decision"] = "expand"
report("import-plan:v2:expand-decision", "SC-1", "static v2 plan", False, import_plan_validators[2].is_valid(sample))
plan_contract_counts[2] += 1
for label, value, expected in v3_cases:
    report("import-plan:v3:" + label, "SC-1", "static v3 plan", expected, import_plan_validators[3].is_valid(value))
    plan_contract_counts[3] += 1

def plan_valid(value, label):
    if not isinstance(value, dict) or value.get("format") != "am-import-plan":
        print(f"import plan {label}: missing or unsupported format")
        return False
    version = value.get("version")
    if type(version) is not int or version not in import_plan_validators:
        print(f"import plan {label}: missing or unsupported version {version!r}")
        return False
    return import_plan_validators[version].is_valid(value)

for label, field, replacement in (("unknown-version", "version", 99), ("missing-version", "version", None),
                                   ("unknown-format", "format", "other"), ("missing-format", "format", None)):
    sample = copy.deepcopy(v2_example)
    if replacement is None:
        del sample[field]
    else:
        sample[field] = replacement
    report("import-plan:dispatch:" + label, "SC-1", "plan version/format dispatch", False, plan_valid(sample, label))
actual_plans = sorted((Path(args.root) / ".cache/import-plan-cases").glob("*.json"))
plan_actual_counts = {1: 0, 2: 0, 3: 0}
for path in actual_plans:
    value = json.loads(path.read_text("utf8"))
    report("import-plan:" + path.stem, "SC-1", "actual scan plan", True,
           plan_valid(value, path.name))
    if isinstance(value, dict) and type(value.get("version")) is int and value["version"] in plan_actual_counts:
        plan_actual_counts[value["version"]] += 1
for version in (1, 2, 3):
    print(f"import plan schema v{version}: {plan_contract_counts[version]} static contract cases and {plan_actual_counts[version]} actual plans checked")
# T-034: declarative input and independently authored import expectations.
corpus_validators = {}
for kind in ("import-case", "expected-import"):
    schema = json.loads((Path(args.root) / f"schemas/{kind}.v1.schema.json").read_text("utf8"))
    jsonschema.Draft202012Validator.check_schema(schema)
    corpus_validators[kind] = jsonschema.Draft202012Validator(schema)
corpus_cases = sorted((Path(args.root) / "tests/fixtures/import").glob("*/case.json"))
seen_scenarios = set()
report("import-corpus:nonempty", "SC-1", "declarative corpus exists", True, bool(corpus_cases))
for path in corpus_cases:
    case = json.loads(path.read_text("utf8"))
    expected = json.loads((path.parent / "expected.json").read_text("utf8"))
    report("import-corpus:case:" + path.parent.name, "SC-1", "case schema", True,
           corpus_validators["import-case"].is_valid(case))
    report("import-corpus:expected:" + path.parent.name, "SC-1", "expectation schema", True,
           corpus_validators["expected-import"].is_valid(expected))
    report("import-corpus:identity:" + path.parent.name, "SC-1", "case directory and expectation identity", True,
           case.get("case") == path.parent.name == expected.get("case"))
    scenarios = case.get("scenarios", [])
    seen_scenarios.update(scenarios)
    report("import-corpus:requirements:" + path.parent.name, "SC-1", "scenarios included in req labels", True,
           bool(scenarios) and set(scenarios).issubset(case.get("req", [])))
    labels = {step.get("name", step["command"]) for step in case["steps"]}
    report("import-corpus:steps:" + path.parent.name, "SC-1", "every step has an expectation", True,
           labels == set(expected["steps"]))
for number in range(1, 17):
    scenario = f"SC-{number}"
    report("import-corpus:coverage:" + scenario, scenario, "scenario has a declarative case", True,
           scenario in seen_scenarios)
if corpus_cases:
    sample = json.loads(corpus_cases[0].read_text("utf8"))
    for label, change in (("extra", {"extra": True}), ("version", {"version": 2}),
                          ("no-scenario", {"scenarios": []}), ("unknown-scenario", {"scenarios": ["SC-17"]})):
        value = copy.deepcopy(sample)
        value.update(change)
        report("import-corpus:negative:" + label, "SC-1", "invalid case rejected", False,
               corpus_validators["import-case"].is_valid(value))
    value = json.loads((corpus_cases[0].parent / "expected.json").read_text("utf8"))
    value["steps"]["scan"]["unexpected"] = True
    report("import-corpus:negative:expected-extra", "SC-1", "unknown expectation projection rejected", False,
           corpus_validators["expected-import"].is_valid(value))
print(f"import corpus schemas: {len(corpus_cases)} cases, {len(seen_scenarios)} scenarios checked")
sys.exit(1 if failures else 0)
