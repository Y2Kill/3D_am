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
sys.exit(1 if failures else 0)
