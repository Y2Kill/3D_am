# Narrow interfaces shared across levels so that services, automation, plugins and
# the application never form cycles (ТЗ §12.1.5, §12.4.4; ADR-0024).
# Header-only: include/am/contracts/*.hpp (ICommandSink, later IJobStore, ICatalog,
# IArchiveReader, IAssetRepository, capability descriptors).
am_add_module(contracts
  DEPENDS am::foundation)
