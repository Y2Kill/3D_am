# Plugin manager (module 22, ТЗ §12.8, ADR-0026): manifest-before-code discovery,
# compatibility, lifecycle, registration through contracts. External SDK: sdk/.
am_add_module(plugin_runtime
  DEPENDS am::foundation am::contracts
  FORBID services application gui)
