# Shared job infrastructure (module 19, ТЗ §12.3): queue, progress, cancel, safe
# points, recovery. Depends only on foundation + contracts; persistence comes
# through contracts::IJobStore implemented in storage (ADR-0024). M3.
am_add_module(jobs
  DEPENDS am::foundation am::contracts
  FORBID storage services automation import)
