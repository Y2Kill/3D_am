# Domain model (module 3, 4, 7 types): asset, files, groups, sources, IDs, revisions,
# manifest <-> JSON mapping and schema validation. Must NOT depend on ZIP or DB
# libraries (ТЗ §12.1.3, §12.2.1). Implemented in M1; INTERFACE until then.
am_add_module(domain
  DEPENDS am::foundation am::contracts
  FORBID storage services gui)
