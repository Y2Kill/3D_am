# Domain model (module 3, 4, 7 types): asset, files, groups, sources, IDs, revisions,
# manifest <-> JSON mapping and schema validation. Must NOT depend on ZIP or DB
# libraries (ТЗ §12.1.3, §12.2.1).
if(AM_WITH_CONTAINER)
  find_package(nlohmann_json CONFIG REQUIRED)
  find_package(jsoncons CONFIG REQUIRED)
  am_add_module(domain
    SOURCES manifest/codec.cpp manifest/schema_validator.cpp
      manifest/semantics.cpp manifest/semantics_syntax.cpp
    DEPENDS am::foundation am::contracts nlohmann_json::nlohmann_json
    FORBID storage services gui)
  target_link_libraries(am_domain PRIVATE jsoncons::jsoncons)
else()
  am_add_module(domain
    DEPENDS am::foundation am::contracts
    FORBID storage services gui)
endif()
