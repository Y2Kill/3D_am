# Индекс документации

Порядок чтения для ознакомления с результатами подготовки:

1. **[reports/01_TZ_evolution_loss_report.md](reports/01_TZ_evolution_loss_report.md)** — что потерялось при эволюции ТЗ v0.1→v1.1 (6 потерь, 2 сужения смысла), предлагаемые правки для v1.2.
2. **[reports/02_collection_findings.md](reports/02_collection_findings.md)** — инженерные выводы из инвентаризации коллекции (масштаб, типовые сценарии импорта, проблемные случаи).
2a. **[reports/03_TZ_v1.3-1.5_vs_prepared.md](reports/03_TZ_v1.3-1.5_vs_prepared.md)** — сверка ТЗ v1.3–v1.5 на потери и сравнение с подготовленными ADR/структурой. **Все рекомендации приняты владельцем 2026-11-06** и внесены (ТЗ v1.6, ADR-0024…0029, раскладка уровней).
3. **[research/libraries.md](research/libraries.md)** — выбор библиотек по разделам §17 ТЗ, лицензии, статусы, окружение машины.
4. **[decisions/ADR_log.md](decisions/ADR_log.md)** — 29 архитектурных решений с таблицей статусов (Принято / Предложено для §17 / Кандидат).
5. **[decisions/open_questions_resolution.md](decisions/open_questions_resolution.md)** — рабочие решения по всем 40 открытым вопросам §16.
6. **Архитектура:**
   - [architecture/overview.md](architecture/overview.md) — уровни ТЗ §12, каталоги, потоки данных
   - [developer/README.md](developer/README.md) — сборка, правила уровней, тесты
   - [specifications/container_manifest_v1.md](specifications/container_manifest_v1.md) — формат ZIP-ассета и манифеста (+ [JSON Schema](../schemas/manifest.v1.schema.json))
   - [architecture/import_pipeline.md](architecture/import_pipeline.md) — конвейер импорта, профиль Telegram, сигнатуры
   - [architecture/jobs_and_sessions.md](architecture/jobs_and_sessions.md) — задания, протокол фиксации ZIP, сессии редактирования
   - [architecture/nfr_targets.md](architecture/nfr_targets.md) — количественные цели (закрывает ТЗ 14.2)
   - SQL-схемы: [index](../schemas/sql/index.sql) · [userdata](../schemas/sql/userdata.sql) · [state](../schemas/sql/state.sql)
7. **[plan/roadmap.md](plan/roadmap.md)** — вехи M0–M11 (+M5P, M-S) с привязкой к критериям приёмки; [plan/coverage_matrix.md](plan/coverage_matrix.md) — матрица 16 сценариев импорта.

Исходные материалы: описание [`tz/description.txt`](tz/description.txt), черновики ТЗ [`tz/drafts/`](tz/drafts/), сканер коллекции [`tools/inventory/inventory_3d_resources.py`](../tools/inventory/inventory_3d_resources.py), результат сканирования [`data/inventory/`](../data/inventory/).

Инструменты аудита ТЗ: `tools/tz_audit/tzdiff.py`, `sentdiff.py`, `verdiff.py` (`python tools/tz_audit/verdiff.py docs/tz/drafts 1.5 1.6`), генераторы `make_v1_2.py`, `make_v1_6.py`. Проверка архитектуры: `tools/arch_check/arch_check.py`.
