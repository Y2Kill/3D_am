# Public application API shared by GUI and CLI (module 24/25 backend, ТЗ §12.1.4).
# INTERFACE until the facade gets its first sources (M1/M2).
set(AM_APP_DEPS am::foundation am::contracts am::domain am::jobs am::storage
                am::processing am::services am::automation am::plugin_runtime)
if(AM_WITH_AI)
  list(APPEND AM_APP_DEPS am::ai)
endif()
am_add_module(application DEPENDS ${AM_APP_DEPS})

if(AM_BUILD_CLI)
  add_subdirectory(cli)
endif()
if(AM_BUILD_GUI)
  add_subdirectory(gui)               # Qt 6 Widgets (official installer, AM_QT_ROOT)
endif()
