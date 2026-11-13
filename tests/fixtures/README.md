# tests/fixtures — синтетические тестовые наборы (ADR-0029, ТЗ §13.4)

Каждый случай — каталог `<case>/`:
- `input/` — малые сгенерированные входные данные;
- `expected.json` — машинно-читаемые ожидания: состав ассетов, пути, SHA-256, исключения с причинами, статусы, ошибки. Схема — `schemas/expected.v1.schema.json` (M1). ID и время проверяются по свойствам, а не побайтово.

Генератор — `tools/fixtures/` (M1). Файлы контрольной коллекции сюда **не копируются** (CLAUDE.md).

Независимый корпус ZIP: [`golden/README.md`](golden/README.md). Исходные манифесты и параметры упаковки — `tools/fixtures/golden/`, генератор — `tools/fixtures/make_golden_zips.py`. `golden_regen_check` сравнивает результат побайтно, `golden_zip` запускает собранный CLI; эталоны проверяются `schema_check` по `schemas/expected.v1.schema.json`.
