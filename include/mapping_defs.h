/*
 * mapping_defs.h - Shared types between kvmapper.exe and kvmapper_hook.dll.
 *
 * v0.10 layout:
 *   - Mapping table (as v0.9) with one new field: sourceFilter
 *   - 8-byte per-instance session token (PID + rand32)
 *   - 256-slot verdict table (one slot per VK) written by the LL
 *     classifier in the exe and read by every injected DLL
 */
#ifndef KVMAPPER_MAPPING_DEFS_H
#define KVMAPPER_MAPPING_DEFS_H

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Tunables ---------- */
#define KV_MAX_TRIGGER_KEYS  4
#define KV_MAX_ACTION_KEYS   4
#define KV_MAX_MAPPINGS      64
#define KV_LABEL_LEN         128

/* The shared-block name. No v0.9 instances exist in production, so
 * we keep the _v1 name. */
#define KV_SHARED_NAME       "kvmapper_shared_v1"

typedef enum {
    KV_SOURCE_ANY    = 0,
    KV_SOURCE_LOCAL  = 1,
    KV_SOURCE_REMOTE = 2
} KVSourceFilter;

typedef enum {
    KV_DISPATCH_PRESSED  = 0,
    KV_DISPATCH_RELEASED = 1
} KVDispatch;

typedef enum {
    KV_ACTION_PRESS_RELEASE = 0,
    KV_ACTION_PRESS         = 1,
    KV_ACTION_RELEASE       = 2
} KVActionType;

typedef struct {
    int32_t  enabled;
    uint8_t  triggerKeys[KV_MAX_TRIGGER_KEYS];
    int32_t  triggerCount;
    int32_t  dispatch;
    int32_t  actionType;
    uint8_t  actionKeys[KV_MAX_ACTION_KEYS];
    int32_t  actionCount;
    int32_t  sourceFilter;   /* v0.10: KVSourceFilter; 0 = ANY (back-compat) */
    char     label[KV_LABEL_LEN];
} KVMapping;

/* Per-event verdict slot. 16 bytes, indexed by VK code (one slot per VK,
 * 256 total). Writer = classifier thread in exe; readers = DLL instances. */
typedef struct {
    uint32_t   vkCode;          /* fingerprint */
    uint32_t   timestampMs;     /* GetTickCount() at classify time */
    uint32_t   flags;           /* mirror of KBDLLHOOKSTRUCT.flags */
    uint32_t   extraInfoLow;    /* low 32 of dwExtraInfo (session token tag) */
} KVEventVerdict;

/* One slot per VK. No (vk ^ time) hashing - GetTickCount's ~15ms
 * granularity makes time-based slot lookup unreliable across a tick
 * boundary. */
#define KV_VERDICT_RING_SIZE 256
#define KV_VERDICT_RING_MASK 0xFF
#define KV_VERDICT_WINDOW_MS 30        /* fingerprint freshness window */

typedef struct {
    /* Mapping table (writers: exe; readers: DLL). */
    volatile LONG sequence;
    int32_t       count;
    int32_t       _pad0;
    KVMapping     mappings[KV_MAX_MAPPINGS];

    /* Session token (written once at exe start). */
    uint32_t      tokenLow;
    uint32_t      tokenPid;

    /* Verdict table (writers: classifier; readers: every DLL). */
    volatile LONG ringGen;
    int32_t       _pad1;
    KVEventVerdict ring[KV_VERDICT_RING_SIZE];
} KVSharedBlock;

/* Compose the dwExtraInfo value to stamp on our SendInput. On x86 only
 * the low 32 bits survive; the classifier matches against extraInfoLow
 * only, so x86 is fine. */
static __inline ULONG_PTR kv_session_extrainfo(uint32_t pid, uint32_t low) {
#if defined(_WIN64)
    return ((ULONG_PTR)pid << 32) | (ULONG_PTR)low;
#else
    (void)pid;
    return (ULONG_PTR)low;
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* KVMAPPER_MAPPING_DEFS_H */
