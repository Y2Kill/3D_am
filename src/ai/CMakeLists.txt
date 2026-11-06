# Optional AI layer (module 23, ТЗ §11.4, ADR-0027): adapter interface, bounded
# context building, result validation and the deterministic TEST adapter
# (ТЗ §11.5.4). Concrete engines (llama.cpp, local services) are plugins.
am_add_module(ai
  DEPENDS am::foundation am::contracts am::domain
  FORBID storage gui)
