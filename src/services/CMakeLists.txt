# Application services (modules 8-13, 16-18): import/analyze, export, catalog and
# index, search, duplicates, preview, external editing sessions.
set(AM_SERVICES_SOURCES import/exclusion_rules.cpp)
if(AM_WITH_CONTAINER)
  list(APPEND AM_SERVICES_SOURCES catalog/indexer.cpp)
endif()
am_add_module(services
  SOURCES ${AM_SERVICES_SOURCES}
  DEPENDS am::foundation am::contracts am::domain am::jobs am::storage am::processing
  FORBID automation application gui)
