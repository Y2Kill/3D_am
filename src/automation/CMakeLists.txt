# Automation (modules 20, 21): command registry, scenario engine (ADR-0027),
# scheduler. Uses jobs; jobs never depend on automation (ТЗ §12.3.4).
am_add_module(automation
  SOURCES command_registry.cpp
  DEPENDS am::foundation am::contracts am::domain am::jobs
  FORBID services application gui)
