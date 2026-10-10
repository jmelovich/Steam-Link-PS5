/*
 * Steam-Link-PS5 - A real malloc heap for the title, backed by direct memory.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The system libc's default heap is a few MiB; a video stream needs far more,
 * and the clean-room runtime offers no way to enlarge it (the native converter
 * exports no symbols, so the usual sceLibcHeap* globals are never read). So we
 * take over the C allocation functions (the build links them with --wrap) and
 * serve them from a TLSF pool over direct memory, growing by mapping more when a
 * pool runs out. One lock guards the allocator; TLSF itself is O(1).
 */

#include "../../third_party/tlsf/tlsf.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align,
                                          int type, int64_t *physical);
extern int sceKernelMapDirectMemory(void **address, size_t len, int protection, int flags,
                                    int64_t physical, size_t align);
extern size_t sceKernelGetDirectMemorySize(void);
extern int sceKernelDebugOutText(int channel, const char *text);

#define PS5_HEAP_CHUNK (128u * 1024u * 1024u)
#define PS5_HEAP_ALIGN 0x4000u
#define PS5_DIRECT_TYPE_WB_ONION 0 /* CPU-cached writeback: a general-purpose heap */
#define PS5_PROT_CPU_RW 0x3

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static tlsf_t g_tlsf;
static int g_ready;

static void note(const char *text)
{
    sceKernelDebugOutText(0, text);
}

/* Map one direct-memory region of at least `need` bytes. */
static void *map_region(size_t need, size_t *mapped)
{
    size_t size = need < PS5_HEAP_CHUNK ? PS5_HEAP_CHUNK : ((need + PS5_HEAP_ALIGN - 1) & ~(size_t)(PS5_HEAP_ALIGN - 1));
    int64_t limit = (int64_t)sceKernelGetDirectMemorySize();
    int64_t physical = 0;
    if (sceKernelAllocateDirectMemory(0, limit, size, PS5_HEAP_ALIGN, PS5_DIRECT_TYPE_WB_ONION,
                                      &physical) != 0)
        return NULL;
    void *address = NULL;
    if (sceKernelMapDirectMemory(&address, size, PS5_PROT_CPU_RW, 0, physical, PS5_HEAP_ALIGN) != 0)
        return NULL;
    *mapped = size;
    return address;
}

/* Caller holds g_lock. Returns 0 on success. */
static int ensure_ready(void)
{
    if (g_ready)
        return 0;
    size_t mapped = 0;
    void *region = map_region(PS5_HEAP_CHUNK, &mapped);
    if (!region)
    {
        note("[SLP5] malloc: initial heap mapping failed\n");
        return -1;
    }
    g_tlsf = tlsf_create_with_pool(region, mapped);
    if (!g_tlsf)
    {
        note("[SLP5] malloc: tlsf_create_with_pool failed\n");
        return -1;
    }
    g_ready = 1;
    note("[SLP5] malloc: heap ready (128 MiB direct, grows on demand)\n");
    return 0;
}

/* Caller holds g_lock. Add another pool big enough for `need`. */
static int grow(size_t need)
{
    size_t mapped = 0;
    /* TLSF needs room for its per-pool overhead on top of the request. */
    void *region = map_region(need + tlsf_pool_overhead() + PS5_HEAP_ALIGN, &mapped);
    if (!region)
    {
        note("[SLP5] malloc: heap grow mapping failed\n");
        return -1;
    }
    if (!tlsf_add_pool(g_tlsf, region, mapped))
    {
        note("[SLP5] malloc: tlsf_add_pool failed\n");
        return -1;
    }
    return 0;
}

void *__wrap_malloc(size_t size)
{
    if (size == 0)
        size = 1;
    pthread_mutex_lock(&g_lock);
    void *result = NULL;
    if (ensure_ready() == 0)
    {
        result = tlsf_malloc(g_tlsf, size);
        if (!result && grow(size) == 0)
            result = tlsf_malloc(g_tlsf, size);
    }
    pthread_mutex_unlock(&g_lock);
    return result;
}

void __wrap_free(void *pointer)
{
    if (!pointer)
        return;
    pthread_mutex_lock(&g_lock);
    if (g_ready)
        tlsf_free(g_tlsf, pointer);
    pthread_mutex_unlock(&g_lock);
}

void *__wrap_calloc(size_t count, size_t size)
{
    size_t total = count * size;
    if (count != 0 && total / count != size)
        return NULL; /* overflow */
    void *result = __wrap_malloc(total == 0 ? 1 : total);
    if (result)
        memset(result, 0, total);
    return result;
}

void *__wrap_realloc(void *pointer, size_t size)
{
    if (!pointer)
        return __wrap_malloc(size);
    if (size == 0)
    {
        __wrap_free(pointer);
        return NULL;
    }
    pthread_mutex_lock(&g_lock);
    void *result = NULL;
    if (g_ready)
    {
        result = tlsf_realloc(g_tlsf, pointer, size);
        if (!result && grow(size) == 0)
            result = tlsf_realloc(g_tlsf, pointer, size);
    }
    pthread_mutex_unlock(&g_lock);
    return result;
}

static void *aligned(size_t alignment, size_t size)
{
    if (alignment < sizeof(void *))
        alignment = sizeof(void *);
    pthread_mutex_lock(&g_lock);
    void *result = NULL;
    if (ensure_ready() == 0)
    {
        result = tlsf_memalign(g_tlsf, alignment, size == 0 ? 1 : size);
        if (!result && grow(size + alignment) == 0)
            result = tlsf_memalign(g_tlsf, alignment, size == 0 ? 1 : size);
    }
    pthread_mutex_unlock(&g_lock);
    return result;
}

void *__wrap_aligned_alloc(size_t alignment, size_t size)
{
    return aligned(alignment, size);
}

void *__wrap_memalign(size_t alignment, size_t size)
{
    return aligned(alignment, size);
}

int __wrap_posix_memalign(void **out, size_t alignment, size_t size)
{
    if (!out)
        return 22; /* EINVAL */
    void *result = aligned(alignment, size);
    if (!result)
        return 12; /* ENOMEM */
    *out = result;
    return 0;
}

/* strdup is a libc function that mallocs; wrap it so its buffer is ours to free. */
char *__wrap_strdup(const char *text)
{
    if (!text)
        return NULL;
    const size_t length = strlen(text) + 1;
    char *copy = __wrap_malloc(length);
    if (copy)
        memcpy(copy, text, length);
    return copy;
}

char *__wrap_strndup(const char *text, size_t max)
{
    if (!text)
        return NULL;
    size_t length = 0;
    while (length < max && text[length] != '\0')
        ++length;
    char *copy = __wrap_malloc(length + 1);
    if (copy)
    {
        memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}
