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
#define GRAFT_VERSION_MINOR 0
#define GRAFT_VERSION_PATCH 1

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

#ifdef __cplusplus
}
#endif

#endif /* GRAFT_GRAFT_H */
