# Application services (modules 8-13, 16-18): import/analyze, export, catalog and
# index, search, duplicates, preview, external editing sessions.
am_add_module(services
  SOURCES import/exclusion_rules.cpp
  DEPENDS am::foundation am::contracts am::domain am::jobs am::storage am::processing
  FORBID automation application gui)
