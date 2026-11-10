# Unicode Character Database 17.0.0

Downloaded on 2026-11-10. The original files are stored byte for byte.

- `UnicodeData.txt`, `CompositionExclusions.txt`, `CaseFolding.txt`,
  `NormalizationTest.txt`: https://www.unicode.org/Public/17.0.0/ucd/
- `license.txt`: https://www.unicode.org/license.txt (Unicode License V3).
- `SHA256SUMS`: SHA-256 of each downloaded file, in sha256sum format.

These data provide canonical normalization and default simple case folding for
the dependency-free foundation layer (ADR-0035). NormalizationTest.txt supplies
the normalization conformance tests. The generator uses only the Python standard
library and verifies every input hash before generating or checking tables:

```text
python -I tools/unicode/gen_unicode_tables.py
python -I tools/unicode/gen_unicode_tables.py --check
```

The generated output is `src/foundation/unicode_tables.inc`. Hangul decomposition
and composition are implemented algorithmically. Folding uses only C/S mappings;
F/T mappings and compatibility decompositions are excluded.
