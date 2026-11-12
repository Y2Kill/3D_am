"""Check embedded user-message coverage without third-party Python modules."""
import argparse
from pathlib import Path
import re
import tomllib

# These exact literals are prefixes/classifiers, never complete user messages.
EXCEPTIONS = {
    "am.app.label.": "Presentation prefix; complete label keys are in en.toml.",
    "am.commit.": "Lower-layer error-family classifier in repository guard.",
    "am.json.": "Codec classifier used to translate strict-JSON error IDs.",
    "am.manifest.": "Semantic/error-family prefix; assembled V3 IDs are checked by the unit test.",
    "am.zip.": "Lower-layer error-family classifier in repository guard.",
}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    catalog = tomllib.loads((args.root / "resources/messages/en.toml").read_text(encoding="utf-8"))
    if not all(isinstance(value, str) and value for value in catalog.values()):
        raise ValueError("Every catalog entry must be nonempty text")
    literals = set()
    for source in (args.root / "src").rglob("*"):
        if source.is_file() and source.suffix in {".cpp", ".hpp", ".h", ".cc", ".ipp", ".in"}:
            literals.update(re.findall(r'"(am\.[a-z0-9_.]+)"', source.read_text(encoding="utf-8")))
    missing = sorted(literals - catalog.keys() - EXCEPTIONS.keys())
    for key in missing:
        print("MISSING", key)
    for key in sorted(literals & EXCEPTIONS.keys()):
        print("EXCLUDED", key, EXCEPTIONS[key])
    print(f"message_catalog_check: {len(catalog)} messages; missing={len(missing)}")
    return bool(missing)

if __name__ == "__main__":
    raise SystemExit(main())
