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
    GRAFT_E_CONFLICT,     /* installed files don't match any version the package knows */

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

/* Masked regions: bytes that differ per install (serials, keys, ...). The
 * installed file's bytes at [old_offset, +old_length) are carried into the
 * new file at [new_offset, +new_length), and are excluded from the source
 * hash. Regions are located when the patch is built (see docs/SCOPE.md). */
enum {
    GRAFT_MASK_EXACT    = 0,  /* lengths equal                                  */
    GRAFT_MASK_PAD      = 1,  /* new_length >= old_length; fill the rest with pad */
    GRAFT_MASK_TRUNCATE = 2   /* new_length <= old_length; keep the leading bytes */
};

typedef struct graft_mask {
    uint64_t old_offset, old_length;
    uint64_t new_offset, new_length;
    uint8_t  mode;  /* GRAFT_MASK_* */
    uint8_t  pad;   /* fill byte for GRAFT_MASK_PAD */
} graft_mask;

/* Executable filters: rewrite references so code that moved diffs well.
 * Lossless on any input; a poor choice only costs ratio.
 *
 * For this (bsdiff-class) encoder, absolute references are what hurt: when
 * code or data moves, every absolute address of it changes, while relative
 * branches between things that moved together don't. Z80 relativizes
 * absolute CALL/JP and data addresses, walking instruction boundaries; on
 * CP/M 2.2 it cut patches by about a third. The rel->abs filters are the
 * classic BCJ transforms, which help compressors but measured 12-36% worse
 * here on ARM64 and x86-64 builds; they are kept for explicit use. */
enum {
    GRAFT_FILTER_NONE   = 0,
    GRAFT_FILTER_X86    = 1,   /* x86-32/64: E8/E9/0F 8x rel32 -> absolute (BCJ) */
    GRAFT_FILTER_X86_16 = 2,   /* 16-bit x86: rel16 -> absolute (BCJ)            */
    GRAFT_FILTER_Z80    = 3,   /* Z80/8080: absolute CALL/JP/data -> relative    */
    GRAFT_FILTER_ARM64  = 4,   /* ARM64: B/BL imm26 -> absolute (BCJ)            */
    GRAFT_FILTER_SLOW32 = 5,   /* SLOW-32: JAL imm21 -> absolute (BCJ)           */
    GRAFT_FILTER_AUTO   = 255  /* try what the header suggests, keep it only if
                                  smaller (currently nothing is auto-detected) */
};

/* The filter graft_diff's GRAFT_FILTER_AUTO would try for this file. */
GRAFT_API uint8_t graft_filter_detect(const void *data, size_t len);

/* Matching. SUFFIX (bsdiff-class) finds approximate matches anywhere and
 * gives the smallest patches, but needs 4-8 bytes of memory per source
 * byte. ROLLING (xdelta-class) indexes the source sparsely: much less
 * memory and time on huge files, larger patches. AUTO uses SUFFIX for
 * sources up to 512 MiB. */
enum { GRAFT_METHOD_AUTO = 0, GRAFT_METHOD_SUFFIX = 1, GRAFT_METHOD_ROLLING = 2 };

typedef struct graft_diff_options {
    uint32_t struct_size;  /* sizeof(graft_diff_options); set by _init */
    int      level;        /* zstd level 1..22                          */
    uint32_t block_size;   /* output bytes per block (memory/ratio trade) */
    const graft_mask *masks;  /* any order; regions must not overlap    */
    size_t   mask_count;      /* 0..4096                                 */
    uint8_t  filter;          /* GRAFT_FILTER_*; default AUTO            */
    uint8_t  method;          /* GRAFT_METHOD_*; default AUTO            */
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
    uint32_t mask_count;
    uint8_t  filter;
    uint64_t source_size;
    uint64_t target_size;
    uint8_t  source_hash[GRAFT_HASH_SIZE];
    uint8_t  target_hash[GRAFT_HASH_SIZE];
} graft_patch_info;

/* Checks whether `source` is what the patch expects (size, and hash of the
 * masked form) without applying it: GRAFT_OK or GRAFT_E_WRONG_SOURCE. */
GRAFT_API graft_status graft_patch_check_source(const graft_reader *patch, const graft_source *source);

/* Reads and validates only the header and mask table. With masks, the
 * hashes are of the normalized files (masked regions zeroed). */
GRAFT_API graft_status graft_patch_read_info(const graft_reader *patch, graft_patch_info *info);

/* ---- Packages: whole directory trees (.gfp, see docs/PACKAGE.md) -------- */

enum { GRAFT_LOG_INFO = 0, GRAFT_LOG_WARN = 1, GRAFT_LOG_ERROR = 2 };
typedef void (*graft_log_fn)(void *ctx, int level, const char *message);

/* Supplies the masked regions for one file, given an old and the new
 * version of it. Store up to `cap` masks and set *count (0 for none). */
typedef graft_status (*graft_mask_fn)(void *ctx, const char *path,
                                      const uint8_t *old_data, size_t old_len,
                                      const uint8_t *new_data, size_t new_len,
                                      graft_mask *masks, size_t cap, size_t *count);

/* Nonzero if a path is user-owned: shipped once, then the user's. The
 * package installs it only where it's missing, and never changes, removes
 * or checks it (configuration, databases, customizable text). */
typedef int (*graft_owned_fn)(void *ctx, const char *path);

/* Picks the filter for one file (GRAFT_FILTER_*; AUTO if unsure). */
typedef uint8_t (*graft_filter_fn)(void *ctx, const char *path);

typedef struct graft_pack_options {
    uint32_t      struct_size;  /* sizeof(graft_pack_options); set by _init */
    int           level;        /* zstd level for deltas and full copies   */
    graft_mask_fn masks_for;    /* NULL: no masks                          */
    void         *masks_ctx;
    graft_log_fn  log;          /* NULL: silent                            */
    void         *log_ctx;
    graft_filter_fn filter_for; /* NULL: AUTO for every file              */
    void         *filter_ctx;
    graft_owned_fn user_owned;  /* NULL: nothing is user-owned            */
    void         *user_ctx;
} graft_pack_options;

GRAFT_API void graft_pack_options_init(graft_pack_options *opts);

/* Writes a package that updates any of the `old_count` old trees to the new
 * tree. Labels name the versions in logs and listings. */
GRAFT_API graft_status graft_pack(const char *new_dir, const char *new_label,
                                  const char *const *old_dirs, const char *const *old_labels,
                                  size_t old_count, const char *out_path,
                                  const graft_pack_options *opts);

enum { GRAFT_UPDATE_DRY_RUN = 1 };

typedef struct graft_update_options {
    uint32_t           struct_size;  /* sizeof(graft_update_options); set by _init */
    uint32_t           flags;        /* GRAFT_UPDATE_*                      */
    graft_apply_limits limits;       /* applied to each file's delta        */
    graft_log_fn       log;
    void              *log_ctx;
} graft_update_options;

GRAFT_API void graft_update_options_init(graft_update_options *opts);

/* Updates the tree at `root_dir` in place. Every file the package touches is
 * checked first; any that matches no known version fails the whole update
 * with GRAFT_E_CONFLICT before anything changes. Changes are journaled: a
 * failure rolls back, and an interrupted update is rolled back the next time
 * graft_update runs on the tree. */
GRAFT_API graft_status graft_update(const char *package_path, const char *root_dir,
                                    const graft_update_options *opts);

/* Logs a package's versions and contents, one line per entry. */
GRAFT_API graft_status graft_package_describe(const char *package_path, graft_log_fn log, void *log_ctx);

/* ---- Versioned archives (.gfa, see docs/ARCHIVE.md) -------------------- */

typedef struct graft_archive graft_archive;

enum { GRAFT_ARCHIVE_WRITE = 1, GRAFT_ARCHIVE_CREATE = 2 };

typedef struct graft_archive_version {
    const char *label;    /* valid until the archive is closed or added to */
    uint64_t    time;     /* seconds since 1970, when it was added */
    uint64_t    file_count;
    uint64_t    total_size;  /* bytes of file content */
} graft_archive_version;

/* Opens an archive. GRAFT_ARCHIVE_CREATE makes an empty one if needed (and
 * implies WRITE). A tail left by an interrupted add is ignored, and trimmed
 * by the next add. Not safe for concurrent writers. */
GRAFT_API graft_status graft_archive_open(const char *path, uint32_t flags, graft_log_fn log, void *log_ctx,
                                          graft_archive **out);
GRAFT_API void         graft_archive_close(graft_archive *a);

GRAFT_API size_t       graft_archive_count(const graft_archive *a);
GRAFT_API graft_status graft_archive_get(const graft_archive *a, size_t index, graft_archive_version *out);

/* Appends the tree at `dir` as a new version. Labels are unique. */
GRAFT_API graft_status graft_archive_add(graft_archive *a, const char *dir, const char *label, int level);

/* Writes a version to `dir`, which must not exist yet. */
GRAFT_API graft_status graft_archive_extract(graft_archive *a, const char *label, const char *dir);

/* Logs the differences between two versions: + added, - removed,
 * M modified, m mode changed, T kind changed. */
GRAFT_API graft_status graft_archive_diff(graft_archive *a, const char *from, const char *to);

/* Writes a package (graft_pack) updating any of the `from` versions to `to`.
 * from_count 0 means every other version. `opts` may be NULL. */
GRAFT_API graft_status graft_archive_mkpatch(graft_archive *a, const char *to, const char *const *from,
                                             size_t from_count, const char *out_path,
                                             const graft_pack_options *opts);

/* Rewrites the archive keeping only the listed versions. */
GRAFT_API graft_status graft_archive_prune(const char *path, const char *const *keep, size_t keep_count,
                                           int level, graft_log_fn log, void *log_ctx);

#ifdef __cplusplus
}
#endif

#endif /* GRAFT_GRAFT_H */
