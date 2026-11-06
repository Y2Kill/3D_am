# Storage level (modules 5, 6, library data, trash; ТЗ §12.2.2, §12.7):
#  archive/   - reading input archives, volume detection (ADR-0011)
#  asset_zip/ - own ZIP container writer/reader, commit protocol (M1, ADR-0005/0006)
#  repository/- AssetRepository: the ONLY writer of live ZIP assets (ADR-0025)
#  db/        - index/userdata/state SQLite access, migrations (M2, ADR-0007)
set(AM_STORAGE_SOURCES archive/volume_detect.cpp)
am_add_module(storage
  SOURCES ${AM_STORAGE_SOURCES}
  DEPENDS am::foundation am::contracts am::domain
  FORBID services processing automation gui)
if(AM_WITH_CONTAINER)
  message(STATUS "am_storage: asset_zip/repository/db are implemented in M1-M2 (docs/plan/roadmap.md)")
endif()
