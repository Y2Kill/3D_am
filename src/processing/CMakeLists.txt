# Processing level (modules 14, 15, 17 handlers): built-in lightweight format
# handlers, analyzers, converters, renderers. Every handler declares capabilities
# (ТЗ §12.5.5). Heavy adapters (Assimp, lib3mf, OCCT) are plugins/ or optional.
am_add_module(processing
  SOURCES formats/stl_probe.cpp
  DEPENDS am::foundation am::contracts am::domain
  FORBID storage services automation gui)
