# Qt 6 Widgets GUI (ADR-0017) — implemented in M5. Placeholder keeps AM_BUILD_GUI=ON configurable.
find_package(Qt6 6.8 COMPONENTS Widgets OpenGLWidgets)
if(NOT Qt6_FOUND)
  message(WARNING "AM_BUILD_GUI=ON but Qt 6 was not found (set AM_QT_ROOT)")
endif()
message(STATUS "am-gui: placeholder until M5 (docs/plan/roadmap.md)")
