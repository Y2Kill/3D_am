"""SQLite contract check for schemas/sql (ADR-0037 item 10, O-16, K-3).

Creates the three databases in memory and checks the exact-uint64 TEXT contract of
index.sql: CHECK constraints, NULL vs '0', numeric order via (length, value), range
filters, index usage and the fact that SQLite SUM() is not exact.
Uses only the Python standard library. Exit codes: 0 pass, 1 failure.
"""
import argparse
import sqlite3
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--root", required=True, help="project source directory")
args = parser.parse_args()

failures = 0


def check(case_id, desc, ok, detail=""):
    global failures
    failures += not ok
    print(f"{'PASS' if ok else 'FAIL'} {case_id:<6} {desc}{(' — ' + detail) if detail and not ok else ''}")


def load(name):
    c = sqlite3.connect(":memory:")
    c.executescript(open(f"{args.root}/schemas/sql/{name}.sql", encoding="utf-8").read())
    return c


for name in ("index", "userdata", "state"):
    try:
        load(name)
        check(f"DDL-{name[0].upper()}", f"{name}.sql executes in a clean database", True)
    except sqlite3.Error as e:
        check(f"DDL-{name[0].upper()}", f"{name}.sql executes in a clean database", False, str(e))

c = load("index")
print(f"sqlite {sqlite3.sqlite_version}")

ASSET = ("a1", 1, "assets/a1.zip", "0", 0, "f" * 64, 1, 0, "2026-11-10T00:00:00Z", "2026-11-10T00:00:00Z", "{}")
c.execute("INSERT INTO assets (asset_id, revision, rel_path, zip_size, zip_mtime_ns, manifest_sha256, format_major,"
          " format_minor, created_at, modified_at, manifest_json) VALUES (?,?,?,?,?,?,?,?,?,?,?)", ASSET)


def insert_file(fid, size, name="x.stl"):
    c.execute("INSERT INTO files (asset_id, file_id, storage_path, name, size, sha256, origin_kind)"
              " VALUES ('a1', ?, 'content/i/x.stl', ?, ?, ?, 'imported')", (fid, name, size, "0" * 64))


GOOD = ["0", "5", "10", "9223372036854775807", "9223372036854775808", "18446744073709551615"]
BAD = ["-1", "01", "1.0", "1e5", "18446744073709551616", "", " 5", "99999999999999999999", "123456789012345678901"]
for i, v in enumerate(GOOD):
    insert_file(f"g{i}", v)
check("U-01", "canonical uint64 values incl. 2^63 and 2^64-1 are accepted",
      c.execute("SELECT count(*) FROM files").fetchone()[0] == len(GOOD))
rejected = []
for v in BAD:
    try:
        insert_file("b" + v, v)
    except sqlite3.IntegrityError:
        rejected.append(v)
check("U-02", f"non-canonical or out-of-range sizes rejected by CHECK ({len(rejected)}/{len(BAD)})",
      len(rejected) == len(BAD), f"accepted: {sorted(set(BAD) - set(rejected))}")
check("U-03", "files.size round-trips exactly as text",
      [r[0] for r in c.execute("SELECT size FROM files WHERE file_id='g5'")] == ["18446744073709551615"])

order = [r[0] for r in c.execute("SELECT size FROM files ORDER BY length(size), size")]
check("U-04", "ORDER BY length(size), size gives unsigned numeric order", order == GOOD, str(order))
lex = [r[0] for r in c.execute("SELECT size FROM files ORDER BY size")]
check("U-05", "plain ORDER BY size is NOT numeric (documented pitfall)", lex != GOOD)

n = "9223372036854775808"
ge = sorted(r[0] for r in c.execute(
    "SELECT size FROM files WHERE length(size) > length(?1) OR (length(size) = length(?1) AND size >= ?1)", (n,)))
check("U-06", "range filter size >= 2^63 via (length, value) is exact",
      ge == sorted(["9223372036854775808", "18446744073709551615"]), str(ge))

total = c.execute("SELECT SUM(size) FROM files").fetchone()[0]
check("U-07", "SQLite SUM() over sizes is not exact (must be computed in C++)", isinstance(total, float), repr(total))

plan = " ".join(r[3] for r in c.execute(
    "EXPLAIN QUERY PLAN SELECT file_id FROM files WHERE name = 'x.stl' COLLATE NOCASE "
    "AND length(size) = 19 AND size = '9223372036854775808'"))
check("U-08", "files_name_size index is used for (name, length(size), size)", "files_name_size" in plan, plan)

c.execute("INSERT INTO input_entries (asset_id, entry_id, input_id, object_id, src_path, src_name, entry_kind, size,"
          " result) VALUES ('a1','e1','i','o','p','n','file',NULL,'unavailable')")
c.execute("INSERT INTO input_entries (asset_id, entry_id, input_id, object_id, src_path, src_name, entry_kind, size,"
          " result) VALUES ('a1','e2','i','o','p','n','file','0','included')")
got = c.execute("SELECT entry_id, quote(size) FROM input_entries ORDER BY entry_id").fetchall()
check("U-09", "input_entries.size keeps NULL (unknown) distinct from '0'", got == [("e1", "NULL"), ("e2", "'0'")], str(got))

plan = " ".join(r[3] for r in c.execute(
    "EXPLAIN QUERY PLAN SELECT entry_id FROM input_entries WHERE crc32 = 'f3c218f0' AND length(size) = 8 "
    "AND size = '25649684'"))
check("U-10", "input_entries_crc_size index is used", "input_entries_crc_size" in plan, plan)

c.execute("UPDATE assets SET content_bytes = NULL, health_detail = 'content_bytes_overflow' WHERE asset_id = 'a1'")
check("U-11", "assets.content_bytes accepts NULL for an overflowing sum (K-3)",
      c.execute("SELECT content_bytes IS NULL FROM assets").fetchone()[0] == 1)
try:
    c.execute("UPDATE assets SET content_bytes = '36893488147419103230' WHERE asset_id = 'a1'")
    check("U-12", "assets.content_bytes rejects a value above uint64", False, "accepted")
except sqlite3.IntegrityError:
    check("U-12", "assets.content_bytes rejects a value above uint64", True)
try:
    c.execute("UPDATE assets SET zip_size = '-1' WHERE asset_id = 'a1'")
    check("U-13", "assets.zip_size rejects a negative value", False, "accepted")
except sqlite3.IntegrityError:
    check("U-13", "assets.zip_size rejects a negative value", True)

u = load("userdata")
fks = u.execute("PRAGMA foreign_key_list(suggestion_decisions)").fetchall()
check("U-14", "userdata.suggestion_decisions has no foreign keys (ADR-0036 item 3)", fks == [], str(fks))

print(f"{'OK' if not failures else 'FAILED'}: {failures} failure(s)")
sys.exit(1 if failures else 0)
