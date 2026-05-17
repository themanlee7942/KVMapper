/*
 * shared_mem.c - exe-side ownership of the KVSharedBlock.
 *
 * v0.10 additions:
 *   - Generates a per-launch session token (PID + 32-bit random).
 *     The classifier and inject.c both read this token; the DLL uses
 *     it to recognise our own SendInput events for re-entrancy.
 *   - Initialises the verdict ring to all-zeros.
 *
 * Seqlock-lite (mapping table writes only):
 *     write rules        memcpy + counter bump (kv_shm_publish)
 *     read in DLL        compare counter, refresh on mismatch
 *
 * The verdict ring is its own concurrency story: single writer (classifier
 * thread in the exe), many readers (every injected DLL instance). Writes
 * are 16-byte aligned and use a release fence for visibility ordering.
 */
#include <windows.h>
#include <string.h>
#include <stdlib.h>

#include "mapping_defs.h"

/* exe-side globals */
static HANDLE         g_hMap   = NULL;
static KVSharedBlock *g_shared = NULL;

KVSharedBlock *kv_shm_create(void) {
    g_hMap = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        sizeof(KVSharedBlock),
        KV_SHARED_NAME);
    if (!g_hMap) return NULL;

    /* Note: A second running instance is impossible to reach here -
     * WinMain checks the single-instance mutex (KV_MUTEX_NAME) BEFORE
     * calling kv_shm_create() and exits if another instance owns it.
     * So GetLastError() == ERROR_ALREADY_EXISTS shouldn't occur in
     * practice, and unconditional zero-init below is safe. */

    g_shared = (KVSharedBlock *)MapViewOfFile(
        g_hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(KVSharedBlock));
    if (!g_shared) {
        CloseHandle(g_hMap);
        g_hMap = NULL;
        return NULL;
    }

    memset(g_shared, 0, sizeof(KVSharedBlock));
    g_shared->sequence = 1;

    /* Session token: PID (high 32) | rand32 (low 32). Generated once
     * per launch and stamped into every SendInput we emit. */
    g_shared->tokenPid = (uint32_t)GetCurrentProcessId();

    /* rand() is fine here - we don't need cryptographic strength,
     * only "different from any other instance running today". Seed with
     * GetTickCount() ^ PID for monotonic-but-spread initial state. */
    srand((unsigned)(GetTickCount() ^ g_shared->tokenPid));
    /* Combine two rand() calls because RAND_MAX is only 15 bits on MSVC. */
    g_shared->tokenLow = ((uint32_t)rand() << 16) ^ (uint32_t)rand()
                       ^ (uint32_t)GetTickCount();
    if (g_shared->tokenLow == 0) g_shared->tokenLow = 1;  /* avoid 0 */

    return g_shared;
}

void kv_shm_destroy(void) {
    if (g_shared) {
        UnmapViewOfFile(g_shared);
        g_shared = NULL;
    }
    if (g_hMap) {
        CloseHandle(g_hMap);
        g_hMap = NULL;
    }
}

/* Publish a mapping-table update. Writer barrier then counter bump. */
void kv_shm_publish(void) {
    if (!g_shared) return;
    MemoryBarrier();
    InterlockedIncrement(&g_shared->sequence);
}

KVSharedBlock *kv_shm_view(void) {
    return g_shared;
}

/* Write a verdict to slot vk (one slot per VK code, 256 total).
 * Called from the classifier thread only - single writer per slot. */
void kv_shm_publish_verdict(uint32_t vkCode,
                            uint32_t timestampMs,
                            uint32_t flags,
                            uint32_t extraInfoLow) {
    if (!g_shared) return;
    int slot = (int)(vkCode & KV_VERDICT_RING_MASK);

    /* Write the slot fields in an order that lets a reader detect
     * a half-completed write via the vkCode/timestamp fingerprint:
     * write metadata first, then vkCode last. */
    g_shared->ring[slot].timestampMs  = timestampMs;
    g_shared->ring[slot].flags        = flags;
    g_shared->ring[slot].extraInfoLow = extraInfoLow;
    MemoryBarrier();
    g_shared->ring[slot].vkCode       = vkCode;

    InterlockedIncrement(&g_shared->ringGen);
}
