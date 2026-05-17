/*
 * hook.c - kvmapper_hook.dll (v0.10)
 *
 * WH_KEYBOARD hook injected into every GUI process.
 *
 * v0.10:
 *   - Classifier publishes per-VK verdict slots in shared memory.
 *     The DLL does ONE read per event (slot = vk) to learn whether
 *     the event is injected (KVM/RDP/SendInput) and whether it
 *     carries our session token.
 *   - g_dispatching thread-local guard is still present as a belt-
 *     and-braces protection against auto-repeat collisions: if the
 *     user auto-repeats a key while we are mid-SendInput, the slot
 *     for that VK might be overwritten by the user's auto-repeat
 *     before our synthetic event reaches WH_KEYBOARD. The TLS flag
 *     catches that case.
 *   - sourceFilter (ANY / LOCAL / REMOTE) is honored per rule.
 *
 * Hot-path discipline (windows-dll-input-hook skill section 4):
 *   - No I/O, no Registry, no MessageBox.
 *   - Per-thread mapping cache (TLS) avoids cross-thread memcpy race.
 *   - Verdict lookup is now a single 16-byte aligned read.
 */
#include <windows.h>
#include <string.h>

#include "mapping_defs.h"

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define KV_TLS _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#  define KV_TLS __thread
#else
#  define KV_TLS __declspec(thread)
#endif

/* ---------- Process-wide state ---------- */
static HINSTANCE      g_dllInst   = NULL;
static HHOOK          g_hook      = NULL;
static HANDLE         g_hMap      = NULL;
static const KVSharedBlock *g_shared = NULL;

/* ---------- Thread-local ---------- */
/* Re-entrancy guard. Set inside inject.c around SendInput so a synthetic
 * event arriving on the same thread is recognised even if the verdict
 * ring slot got overwritten between LL classify and WH_KEYBOARD dispatch
 * (rapid auto-repeat collisions). */
KV_TLS int g_dispatching = 0;

/* Per-thread mapping cache. */
static KV_TLS KVSharedBlock g_local;
static KV_TLS LONG          g_seq = -1;
static KV_TLS BYTE          g_heldKeys[256];

/* ---------- Exports ---------- */
__declspec(dllexport) LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam);
__declspec(dllexport) BOOL install_hook(void);
__declspec(dllexport) BOOL uninstall_hook(void);
__declspec(dllexport) BOOL reload_mappings(void);

extern void kv_fire_action(const KVMapping *m);

/* Accessor used by inject.c to read the session token. */
const KVSharedBlock *kv_dll_shared(void) { return g_shared; }

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_dllInst = hInst;
        DisableThreadLibraryCalls(hInst);
    }
    return TRUE;
}

static void kv_refresh_local(void) {
    if (!g_shared) return;
    LONG seen = g_shared->sequence;
    memcpy(&g_local, (const void*)g_shared, sizeof(KVSharedBlock));
    g_seq = seen;
}

BOOL install_hook(void) {
    if (g_hook) return TRUE;

    g_hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, KV_SHARED_NAME);
    if (!g_hMap) return FALSE;

    g_shared = (const KVSharedBlock *)MapViewOfFile(
        g_hMap, FILE_MAP_READ, 0, 0, sizeof(KVSharedBlock));
    if (!g_shared) {
        CloseHandle(g_hMap);
        g_hMap = NULL;
        return FALSE;
    }

    g_hook = SetWindowsHookExA(WH_KEYBOARD, KeyboardProc, g_dllInst, 0);
    if (!g_hook) {
        UnmapViewOfFile(g_shared);
        CloseHandle(g_hMap);
        g_shared = NULL;
        g_hMap = NULL;
        return FALSE;
    }
    return TRUE;
}

BOOL uninstall_hook(void) {
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = NULL;
    }
    if (g_shared) {
        UnmapViewOfFile(g_shared);
        g_shared = NULL;
    }
    if (g_hMap) {
        CloseHandle(g_hMap);
        g_hMap = NULL;
    }
    return TRUE;
}

BOOL reload_mappings(void) {
    if (!g_shared) return FALSE;
    kv_refresh_local();
    return TRUE;
}

/* Single-read verdict lookup: slot is indexed by VK directly, so there's
 * no collision between different keys and no need to probe a window of
 * timestamps. The freshness check still rejects stale slots (older than
 * KV_VERDICT_WINDOW_MS) so a verdict left over from minutes ago can't
 * influence current event handling. */
static int kv_lookup_verdict(BYTE vk, int *outInjected, int *outIsOurs) {
    *outInjected = 0;
    *outIsOurs   = 0;
    if (!g_shared) return 0;

    const KVEventVerdict *v = &g_shared->ring[(unsigned)vk & KV_VERDICT_RING_MASK];
    /* Acquire-style read on x86/x64 is implicit via the memory model.
     * Compiler reads vkCode first (use it as the validity check), then
     * the other fields; on TSO this is the program order seen by other
     * cores too. */
    if (v->vkCode != (uint32_t)vk) return 0;

    uint32_t now = (uint32_t)GetTickCount();
    if ((now - v->timestampMs) > KV_VERDICT_WINDOW_MS) return 0;

    *outInjected = (v->flags & LLKHF_INJECTED) ? 1 : 0;
    *outIsOurs   = (v->extraInfoLow == g_shared->tokenLow) ? 1 : 0;
    return 1;
}

LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0) {
        return CallNextHookEx(NULL, code, wParam, lParam);
    }
    if (!g_shared) {
        return CallNextHookEx(NULL, code, wParam, lParam);
    }

    /* Belt-and-braces guard: if this thread is mid-SendInput, pass through
     * unconditionally. Catches the rare auto-repeat collision where the
     * verdict ring slot got overwritten by the user's keystroke between
     * LL classify and WH_KEYBOARD dispatch. */
    if (g_dispatching) {
        return CallNextHookEx(NULL, code, wParam, lParam);
    }

    BOOL keyUp = (BOOL)(((DWORD)(DWORD_PTR)lParam >> 31) & 1u);
    BYTE vk    = (BYTE)(wParam & 0xFFu);

    int isInjected = 0, isOurs = 0;
    kv_lookup_verdict(vk, &isInjected, &isOurs);

    g_heldKeys[vk] = keyUp ? 0 : 1;

    /* Our own event - pass through, no rule matching. */
    if (isOurs) {
        return CallNextHookEx(NULL, code, wParam, lParam);
    }

    LONG seq = g_shared->sequence;
    if (seq != g_seq) {
        kv_refresh_local();
    }

    for (int32_t i = 0; i < g_local.count && i < KV_MAX_MAPPINGS; i++) {
        const KVMapping *m = &g_local.mappings[i];
        if (!m->enabled) continue;

        /* Source filter check. */
        if (m->sourceFilter == KV_SOURCE_LOCAL  && isInjected) continue;
        if (m->sourceFilter == KV_SOURCE_REMOTE && !isInjected) continue;

        BOOL wantUp = (m->dispatch == KV_DISPATCH_RELEASED);
        if (wantUp != keyUp) continue;

        BOOL changedIsTrigger = FALSE;
        for (int32_t j = 0; j < m->triggerCount; j++) {
            if (m->triggerKeys[j] == vk) { changedIsTrigger = TRUE; break; }
        }
        if (!changedIsTrigger) continue;

        BOOL match = TRUE;
        for (int32_t j = 0; j < m->triggerCount; j++) {
            BYTE tk = m->triggerKeys[j];
            if (wantUp && tk == vk) continue;
            if (!g_heldKeys[tk]) { match = FALSE; break; }
        }
        if (!match) continue;

        kv_fire_action(m);
        return 1;
    }

    return CallNextHookEx(NULL, code, wParam, lParam);
}
