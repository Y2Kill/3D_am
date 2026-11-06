# am_add_module(<name> [SOURCES ...] [DEPENDS ...] [FORBID ...])
#
# Creates library am_<name> (alias am::<name>) for one architecture level
# (ADR-0024, ТЗ §12.6). Public headers live in include/am/<name>/.
#  - With SOURCES: STATIC library (ТЗ §12.6.5: internal libraries are static).
#  - Without SOURCES: INTERFACE library — header-only contracts or a level that
#    is planned but not implemented yet, so the dependency graph is already real.
# FORBID lists levels this one must never link (direct check at configure time);
# transitive rules and #include rules are checked by tools/arch_check/arch_check.py.
function(am_add_module name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPENDS;FORBID" ${ARGN})
  set(target am_${name})
  foreach(forbidden IN LISTS ARG_FORBID)
    if("am::${forbidden}" IN_LIST ARG_DEPENDS OR "am_${forbidden}" IN_LIST ARG_DEPENDS)
      message(FATAL_ERROR "Layering violation: am_${name} must not depend on am_${forbidden}")
    endif()
  endforeach()

  if(ARG_SOURCES)
    add_library(${target} STATIC ${ARG_SOURCES})
    set(scope PUBLIC)
  else()
    add_library(${target} INTERFACE)
    set(scope INTERFACE)
  endif()
  add_library(am::${name} ALIAS ${target})
  target_include_directories(${target} ${scope}
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/sdk/include>)
  target_compile_features(${target} ${scope} cxx_std_20)
  if(ARG_DEPENDS)
    target_link_libraries(${target} ${scope} ${ARG_DEPENDS})
  endif()
  if(ARG_SOURCES)
    am_apply_defaults(${target})
  endif()
  set_property(GLOBAL APPEND PROPERTY AM_MODULES ${target})
endfunction()
