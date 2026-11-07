# Индекс документации

Порядок чтения для ознакомления с результатами подготовки:

1. **[reports/01_TZ_evolution_loss_report.md](reports/01_TZ_evolution_loss_report.md)** — что потерялось при эволюции ТЗ v0.1→v1.1 (6 потерь, 2 сужения смысла), предлагаемые правки для v1.2.
2. **[reports/02_collection_findings.md](reports/02_collection_findings.md)** — инженерные выводы из инвентаризации коллекции (масштаб, типовые сценарии импорта, проблемные случаи).
2a. **[reports/03_TZ_v1.3-1.5_vs_prepared.md](reports/03_TZ_v1.3-1.5_vs_prepared.md)** — сверка ТЗ v1.3–v1.5 на потери и сравнение с подготовленными ADR/структурой. **Все рекомендации приняты владельцем 2026-11-06** и внесены (ТЗ v1.6, ADR-0024…0029, раскладка уровней).
2b. **[reports/04_TZ_v1.7_vs_prepared.md](reports/04_TZ_v1.7_vs_prepared.md)** — ТЗ v1.7 (нефункциональные требования §14): потерь нет; сверка с подготовленным, рекомендации Р-1…Р-9. **Все приняты владельцем 2026-11-06** и внесены (ADR-0030…0034, модель ошибок, план измерений, матрица приёмки, реестр версий).
2c. **[reports/05_TZ_v1.8_format_vs_prepared.md](reports/05_TZ_v1.8_format_vs_prepared.md)** — ТЗ v1.8 (§19: этапы P0–P3, формат ZIP MF-01…MF-12) и отчёт самосверки владельца: сравнение с подготовленным. **Все рекомендации приняты владельцем 2026-11-06** и внесены (ADR-0035, спецификация и черновик схемы формата 1.0, roadmap по P0–P3).
2d. **[reports/06_R01-R09_vs_prepared.md](reports/06_R01-R09_vs_prepared.md)** — сверка 44 утверждённых уточнений формата R-01…R-09 с подготовленным. **Шесть рекомендаций приняты владельцем 2026-11-07** ([решения](tz/R01-R09_owner_decisions_2026-11-07.md)) и внесены: ADR-0036, спецификация, черновая схема, SQL, тест `schema_check`, спайк S7. Раздел 9 — фактические результаты применения.
3. **[research/libraries.md](research/libraries.md)** — выбор библиотек по разделам §17 ТЗ, лицензии, статусы, окружение машины.
4. **[decisions/ADR_log.md](decisions/ADR_log.md)** — 36 архитектурных решений с таблицей статусов (Принято / Предложено для §17 / Кандидат).
5. **[decisions/open_questions_resolution.md](decisions/open_questions_resolution.md)** — рабочие решения по всем 40 открытым вопросам §16.
6. **Архитектура:**
   - [architecture/overview.md](architecture/overview.md) — уровни ТЗ §12, каталоги, потоки данных
   - [developer/README.md](developer/README.md) — сборка, правила уровней, тесты
   - [specifications/container_manifest_v1.md](specifications/container_manifest_v1.md) — формат ZIP-ассета 1.0 по ТЗ §19.4, рабочая формализация (+ [черновик JSON Schema](../schemas/manifest-1.0.draft.schema.json), [пример](../schemas/examples/manifest-1.0.example.json))
   - [specifications/versioning.md](specifications/versioning.md) — реестр версий девяти форматов и интерфейсов (ТЗ §14.7.1)
   - [architecture/import_pipeline.md](architecture/import_pipeline.md) — конвейер импорта, профиль Telegram, сигнатуры
   - [architecture/jobs_and_sessions.md](architecture/jobs_and_sessions.md) — задания, протокол фиксации ZIP, сессии редактирования
   - [architecture/nfr_measurement_plan.md](architecture/nfr_measurement_plan.md) — план измерений нефункциональных показателей; гипотезы, а не нормативы (ТЗ §14, отчёт 04 Р-1)
   - SQL-схемы: [index](../schemas/sql/index.sql) · [userdata](../schemas/sql/userdata.sql) · [state](../schemas/sql/state.sql)
7. **[plan/roadmap.md](plan/roadmap.md)** — этапы P0–P3 (ТЗ §19.1) и вехи M0–M11 (+M3a/b, M5P, M6a/b, M-S) с привязкой к критериям приёмки; [plan/coverage_matrix.md](plan/coverage_matrix.md) — матрица 16 сценариев импорта; [plan/acceptance_matrix.md](plan/acceptance_matrix.md) — матрица приёмки (Requirement ID, пять статусов ТЗ §14.9.12).

Исходные материалы: описание [`tz/description.txt`](tz/description.txt), черновики ТЗ [`tz/drafts/`](tz/drafts/), материалы владельца к v1.8 ([`tz/AUDIT_REPORT.md`](tz/AUDIT_REPORT.md), [`tz/asset-format-consensus-v0.2.md`](tz/asset-format-consensus-v0.2.md), устаревшая [`tz/asset-format-working-spec-v0.1.md`](tz/asset-format-working-spec-v0.1.md), архивы в `tz/`), уточнения формата [`tz/R01-R09_approved_handoff.md`](tz/R01-R09_approved_handoff.md) и [решения владельца по отчёту 06](tz/R01-R09_owner_decisions_2026-11-07.md), сканер коллекции [`tools/inventory/inventory_3d_resources.py`](../tools/inventory/inventory_3d_resources.py), результат сканирования [`data/inventory/`](../data/inventory/).

Инструменты аудита ТЗ: `tools/tz_audit/tzdiff.py`, `sentdiff.py`, `verdiff.py` (`python tools/tz_audit/verdiff.py docs/tz/drafts 1.5 1.6`), генераторы `make_v1_2.py`, `make_v1_6.py`. Проверка архитектуры: `tools/arch_check/arch_check.py`. Проверка черновой схемы манифеста: `tests/schema/schema_check.py` (ctest `schema_check`).
