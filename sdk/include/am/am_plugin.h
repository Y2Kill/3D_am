/*
 * AM Plugin ABI, version 1 — DRAFT / CANDIDATE (ADR-0009, ADR-0026, ТЗ §12.8.4).
 * Not frozen until spike S6 (other MSVC toolset + clang-cl, buffers, callbacks,
 * load failure, unload on stop). Must not include any internal am/ header.
 *
 * STABILITY CONTRACT
 *  - Pure C, no C++ types, no exceptions across the boundary.
 *  - Every struct starts with `uint32_t struct_size`; new fields are only ever
 *    appended. Readers must check struct_size before touching a field.
 *  - Strings are UTF-8, passed as (pointer, length) — never assumed NUL-terminated.
 *  - Memory: whoever allocates frees. Output buffers are filled through
 *    host-provided writer callbacks, so allocators never cross the boundary.
 *  - A plugin exports exactly one symbol: am_plugin_entry.
 *
 * OWNERSHIP (ADR-0026 p.4, ТЗ §12.8.5)
 *  - am_str passed INTO a call is borrowed for the duration of that call only.
 *  - Descriptors returned by the plugin (am_plugin_v1 and everything it points
 *    to) are owned by the plugin and must stay valid until on_unload returns.
 *  - am_input_stream / am_output_sink / am_host_v1 / op_ctx are owned by the
 *    host; a plugin must not keep them after the call that received them
 *    returns (op_ctx: after the operation finishes or is cancelled).
 *  - Callbacks registered with the host stay valid until the plugin enters
 *    the draining state (on_drain) — no calls arrive after on_drain returns.
 *  - Neither side ever frees memory allocated by the other.
 *
 * LIFECYCLE (ADR-0026 p.3): discovered -> checked -> loaded (am_plugin_entry,
 * on_load) -> registered -> active -> draining (on_drain: no new calls) ->
 * stopped -> unloaded (on_unload). Hot unloading is not promised.
 *
 * Plugin metadata (id, version, capabilities, requires_api) lives in a
 * plugin.toml next to the binary, so the host can list/enable/disable plugins
 * without loading untrusted code (ТЗ 3.3.3–3.3.4).
 */
#ifndef AM_PLUGIN_H
#define AM_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  define AM_PLUGIN_EXPORT __declspec(dllexport)
#else
#  define AM_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#define AM_ABI_VERSION_MAJOR 1u
#define AM_ABI_VERSION_MINOR 0u

typedef struct am_str { const char* data; size_t size; } am_str;

typedef enum am_status {
  AM_OK = 0,
  AM_ERR_INVALID = 1,
  AM_ERR_UNSUPPORTED = 2,
  AM_ERR_IO = 3,
  AM_ERR_CORRUPT = 4,
  AM_ERR_CANCELLED = 5,
  AM_ERR_INTERNAL = 6
} am_status;

typedef enum am_log_level { AM_LOG_DEBUG = 0, AM_LOG_INFO = 1, AM_LOG_WARN = 2, AM_LOG_ERROR = 3 } am_log_level;

/* Streams provided by the host (e.g. a ZIP entry or a temp file). */
typedef struct am_input_stream {
  uint32_t struct_size;
  void* ctx;
  /* returns bytes read, 0 on EOF, (size_t)-1 on error */
  size_t (*read)(void* ctx, void* buf, size_t len);
  /* total size or UINT64_MAX if unknown */
  uint64_t size;
  am_str name;          /* file name inside the asset, informational */
} am_input_stream;

typedef struct am_output_sink {
  uint32_t struct_size;
  void* ctx;
  am_status (*write)(void* ctx, const void* buf, size_t len);
} am_output_sink;

/* Host services available to plugins. */
typedef struct am_host_v1 {
  uint32_t struct_size;
  uint32_t abi_major, abi_minor;
  void (*log)(am_log_level level, am_str plugin_id, am_str message);
  /* Returns non-zero when the current operation was cancelled. */
  int (*is_cancelled)(void* op_ctx);
  void (*progress)(void* op_ctx, uint64_t done, uint64_t total /* UINT64_MAX = unknown */);
  /* Plugin settings (namespaced by plugin id): JSON in, JSON out through sink. */
  am_status (*get_settings_json)(am_str plugin_id, am_output_sink* out);
} am_host_v1;

/* ---- Capability: metadata extraction ("metadata.extract") ---------------- */
typedef struct am_metadata_extractor_v1 {
  uint32_t struct_size;
  am_str id;                      /* "x.acme.ctb" */
  /* Cheap check by name and first bytes. Returns 1 if this extractor applies. */
  int (*accepts)(am_str file_name, const uint8_t* header, size_t header_len);
  /* Writes a JSON object to `out`; it is stored under manifest ext["x.<plugin-id>"]. */
  am_status (*extract)(void* op_ctx, am_input_stream* in, am_output_sink* out_json);
} am_metadata_extractor_v1;

/* ---- Capability: preview generation ("preview.generate") ---------------- */
typedef struct am_preview_request_v1 {
  uint32_t struct_size;
  uint32_t max_width, max_height;
  am_str params_json;             /* camera, light, background… (ТЗ 8.2.3) */
} am_preview_request_v1;

typedef struct am_preview_generator_v1 {
  uint32_t struct_size;
  am_str id;
  int (*accepts)(am_str file_name, const uint8_t* header, size_t header_len);
  /* Writes a PNG image to `out_png`. */
  am_status (*generate)(void* op_ctx, am_input_stream* in, const am_preview_request_v1* req,
                        am_output_sink* out_png);
} am_preview_generator_v1;

/* ---- Capability: command ("command") ------------------------------------ */
/* Permission levels (ТЗ §11.1.3, ADR-0027): read < prepare < modify. */
typedef enum am_permission { AM_PERM_READ = 0, AM_PERM_PREPARE = 1, AM_PERM_MODIFY = 2 } am_permission;
/* Declared idempotency (ТЗ §11.3.6). */
typedef enum am_idempotency { AM_IDEMP_NONE = 0, AM_IDEMP_BY_OP_ID = 1, AM_IDEMP_NATURAL = 2 } am_idempotency;

typedef struct am_command_v1 {
  uint32_t struct_size;
  am_str id;                      /* "x.acme.do_something" */
  am_str title;                   /* UI text, UTF-8 */
  am_str contexts;                /* comma-separated: asset,file,selection,library */
  am_str default_shortcut;        /* "" if none */
  uint32_t permission;            /* am_permission */
  uint32_t dangerous;             /* non-zero: explicit confirmation required */
  uint32_t scriptable;            /* non-zero: usable from scenarios */
  uint32_t idempotency;           /* am_idempotency */
  /* args/result are JSON. */
  am_status (*run)(void* op_ctx, am_str args_json, am_output_sink* out_json);
} am_command_v1;

/* ---- Plugin descriptor --------------------------------------------------- */
typedef struct am_plugin_v1 {
  uint32_t struct_size;
  uint32_t abi_major, abi_minor;  /* ABI the plugin was built against */
  am_str id;                      /* must match plugin.toml */
  am_str version;                 /* semver */

  const am_metadata_extractor_v1* const* extractors; size_t extractor_count;
  const am_preview_generator_v1* const* previewers;  size_t previewer_count;
  const am_command_v1* const* commands;              size_t command_count;

  /* Optional lifecycle hooks (may be NULL). */
  am_status (*on_load)(const am_host_v1* host);
  void (*on_unload)(void);
  /* Appended in draft v1.0 (ADR-0026): stop accepting new calls, finish or
     cancel running ones. May be NULL. */
  void (*on_drain)(void);
} am_plugin_v1;

typedef const am_plugin_v1* (*am_plugin_entry_fn)(const am_host_v1* host);
#define AM_PLUGIN_ENTRY_SYMBOL "am_plugin_entry"

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* AM_PLUGIN_H */
