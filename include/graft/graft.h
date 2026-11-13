/*
 * graft.h: the public C API of libgraft.
 *
 * Stable C ABI over a C++20 implementation. Every function reports failure
 * through graft_status; none throws, aborts, or prints.
 */
#ifndef GRAFT_GRAFT_H
#define GRAFT_GRAFT_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(GRAFT_SHARED)
#  if defined(GRAFT_BUILDING)
#    define GRAFT_API __declspec(dllexport)
#  else
#    define GRAFT_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define GRAFT_API __attribute__((visibility("default")))
#else
#  define GRAFT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Versions ----------------------------------------------------------- */

#define GRAFT_VERSION_MAJOR 0
#define GRAFT_VERSION_MINOR 1
#define GRAFT_VERSION_PATCH 0

/* "MAJOR.MINOR.PATCH" of the linked library. */
GRAFT_API const char *graft_version(void);

/* Human-readable versions of the libraries libgraft was built with,
 * e.g. "zstd 1.5.7, libsais 2.12.2, blake3 1.8.7". */
GRAFT_API const char *graft_dependency_versions(void);

/* ---- Status ------------------------------------------------------------- */

typedef enum graft_status {
    GRAFT_OK = 0,
    GRAFT_E_INVALID_ARG,  /* a NULL or out-of-range argument          */
    GRAFT_E_NOMEM,        /* allocation failed                        */
    GRAFT_E_IO,           /* read or write failed                     */
    GRAFT_E_CORRUPT,      /* malformed input (patch, archive, ...)    */
    GRAFT_E_LIMIT,        /* input exceeds a configured safety limit  */
    GRAFT_E_WRONG_SOURCE, /* source doesn't match what the patch expects */
    GRAFT_E_UNSUPPORTED,  /* valid, but a format version or feature we lack */

    /* Not a status. Forces the enum to span int, so C++ code receiving a
     * value from a newer library or a C caller doesn't hit undefined behavior. */
    GRAFT_STATUS_MAX_ENUM_ = 0x7fffffff
} graft_status;

/* Static string for a status; never NULL. */
GRAFT_API const char *graft_status_string(graft_status status);

/* ---- Hashing (BLAKE3-256) ----------------------------------------------- */

#define GRAFT_HASH_SIZE 32

typedef struct graft_hasher graft_hasher;

/* One-shot. `data` may be NULL only when `len` is 0. */
GRAFT_API graft_status graft_hash(const void *data, size_t len,
                                  uint8_t out[GRAFT_HASH_SIZE]);

/* Streaming. graft_hasher_final may be called more than once; it does not
 * reset the state. graft_hasher_free(NULL) is a no-op. */
GRAFT_API graft_status graft_hasher_new(graft_hasher **out);
GRAFT_API graft_status graft_hasher_update(graft_hasher *h, const void *data, size_t len);
GRAFT_API graft_status graft_hasher_final(const graft_hasher *h, uint8_t out[GRAFT_HASH_SIZE]);
GRAFT_API void         graft_hasher_free(graft_hasher *h);

/* ---- I/O callbacks ------------------------------------------------------ */

/* Random-access source (the old version). read_at must fill exactly `len`
 * bytes; the library never asks past `size`. */
typedef struct graft_source {
    void *ctx;
    uint64_t size;
    graft_status (*read_at)(void *ctx, uint64_t offset, void *buf, size_t len);
} graft_source;

/* Sequential input. Stores 0..cap bytes in *got; *got == 0 means end of input. */
typedef struct graft_reader {
    void *ctx;
    graft_status (*read)(void *ctx, void *buf, size_t cap, size_t *got);
} graft_reader;

/* Sequential output. write must accept all `len` bytes or fail. */
typedef struct graft_sink {
    void *ctx;
    graft_status (*write)(void *ctx, const void *buf, size_t len);
} graft_sink;

/* A source over caller-owned memory, which must outlive its use. */
GRAFT_API graft_source graft_source_from_memory(const void *data, size_t len);

/* ---- Making patches (.gft, see docs/FORMAT.md) -------------------------- */

#define GRAFT_DEFAULT_LEVEL      19
#define GRAFT_DEFAULT_BLOCK_SIZE (8u << 20)   /*  8 MiB of output per block */
#define GRAFT_MIN_BLOCK_SIZE     64u
#define GRAFT_MAX_BLOCK_SIZE     (1u << 30)

typedef struct graft_diff_options {
    uint32_t struct_size;  /* sizeof(graft_diff_options); set by _init */
    int      level;        /* zstd level 1..22                          */
    uint32_t block_size;   /* output bytes per block (memory/ratio trade) */
} graft_diff_options;

GRAFT_API void graft_diff_options_init(graft_diff_options *opts);

/* Writes a patch turning `source` into `target` to `out`. Both inputs are
 * held in memory; the encoder also needs 4-8 bytes per source byte for its
 * suffix array. `opts` may be NULL for defaults. */
GRAFT_API graft_status graft_diff(const void *source, size_t source_len,
                                  const void *target, size_t target_len,
                                  const graft_diff_options *opts,
                                  const graft_sink *out);

/* ---- Applying patches --------------------------------------------------- */

#define GRAFT_DEFAULT_MAX_STREAM (64u << 20)  /* 64 MiB */

typedef struct graft_apply_limits {
    uint32_t struct_size;      /* sizeof(graft_apply_limits); set by _init */
    uint64_t max_stream;       /* reject patches whose blocks need more    */
    uint64_t max_target_size;  /* reject patches producing more            */
} graft_apply_limits;

GRAFT_API void graft_apply_limits_init(graft_apply_limits *limits);

/* Streams the patch from `patch`, writing the target to `out`. Memory is
 * bounded by about 4 x max_stream. Output is provisional until this returns
 * GRAFT_OK: the patch and target hashes are checked last. `limits` may be
 * NULL for defaults. */
GRAFT_API graft_status graft_apply(const graft_source *source,
                                   const graft_reader *patch,
                                   const graft_sink *out,
                                   const graft_apply_limits *limits);

typedef struct graft_patch_info {
    uint16_t format_version;
    uint16_t flags;
    uint32_t max_stream;
    uint64_t source_size;
    uint64_t target_size;
    uint8_t  source_hash[GRAFT_HASH_SIZE];
    uint8_t  target_hash[GRAFT_HASH_SIZE];
} graft_patch_info;

/* Reads and validates only the header. */
GRAFT_API graft_status graft_patch_read_info(const graft_reader *patch, graft_patch_info *info);

#ifdef __cplusplus
}
#endif

#endif /* GRAFT_GRAFT_H */
