#ifndef ZGEC_INTERNAL_H
#define ZGEC_INTERNAL_H

/* Shared internals deduplicated from decode.c / encode.c / seq.c /
 * dict.c / common.c. Not part of the public API; for in-tree use only.
 */

#include "zgec_common.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
typedef struct { CRITICAL_SECTION cs; int ok; } zgec_mu;
#else
#include <pthread.h>
#include <unistd.h>
typedef struct { pthread_mutex_t mu; int ok; } zgec_mu;
#endif

/* Worker cap (was ZGEC_DEC_MAX_WORKERS / ZGEC_ENC_MAX_WORKERS, both 64). */
#define ZGEC_MAX_WORKERS 64u

static inline void zgec_mu_init(zgec_mu *m)
{
    if (m == NULL) return;
    m->ok = 0;
#if defined(_WIN32)
    InitializeCriticalSection(&m->cs);
    m->ok = 1;
#else
    if (pthread_mutex_init(&m->mu, NULL) == 0) m->ok = 1;
#endif
}

static inline void zgec_mu_destroy(zgec_mu *m)
{
    if (m == NULL || m->ok == 0) return;
#if defined(_WIN32)
    DeleteCriticalSection(&m->cs);
#else
    (void)pthread_mutex_destroy(&m->mu);
#endif
    m->ok = 0;
}

static inline void zgec_mu_lock(zgec_mu *m)
{
    if (m == NULL || m->ok == 0) return;
#if defined(_WIN32)
    EnterCriticalSection(&m->cs);
#else
    (void)pthread_mutex_lock(&m->mu);
#endif
}

static inline void zgec_mu_unlock(zgec_mu *m)
{
    if (m == NULL || m->ok == 0) return;
#if defined(_WIN32)
    LeaveCriticalSection(&m->cs);
#else
    (void)pthread_mutex_unlock(&m->mu);
#endif
}

static inline unsigned zgec_cpu_count(void)
{
#if defined(_WIN32)
    SYSTEM_INFO si;
    memset(&si, 0, sizeof(si));
    GetSystemInfo(&si);
    if (si.dwNumberOfProcessors >= 1u && si.dwNumberOfProcessors <= 1024u)
        return (unsigned)si.dwNumberOfProcessors;
    return 1u;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n >= 1L && n <= 1024L) return (unsigned)n;
    return 1u;
#endif
}

/* Histogram normalisation to 2^al (Annex A). Canonical implementation
 * lives in src/seq.c; src/encode.c used to carry a byte-identical copy. */
int zgec_normalize_counts(int16_t *counts, const uint32_t *hist,
                          int nsym, int al);

/* Stream sentinel check (V5): shared by decode.c and dict.c. */
static inline zgec_err zgec_check_stream_sentinel(const uint8_t *s, size_t n)
{
    if (s == NULL && n > 0) return ZGEC_ERR_TRUNCATED;
    if (n == 0) return ZGEC_ERR_STREAM_SIZE;
    if (s[n - 1] == 0) return ZGEC_ERR_BITSTREAM_SENTINEL;
    return ZGEC_OK;
}

/* Shuffle column length shared by filter apply/inverse (section 7.5). */
static inline size_t zgec_filter_shuffle_count(size_t rows, size_t rem, size_t c)
{
    return rows + ((c < rem) ? 1u : 0u);
}

#endif /* ZGEC_INTERNAL_H */
