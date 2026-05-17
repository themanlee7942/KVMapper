/*
 * classifier.c - LL keyboard hook in kvmapper.exe.
 *
 * v0.10.3: now does BOTH classification AND remap.
 *
 * Why move remap here:
 *   - The original design used WH_KEYBOARD DLL injection (kvmapper_hook.dll)
 *     for remapping. That works in many cases but fails for UWP apps,
 *     elevated processes (when KVMapper isn't elevated), AppContainer
 *     apps (Edge, Chrome sandboxed renderer), and a handful of other
 *     hardened targets.
 *   - A WH_KEYBOARD_LL hook here runs system-wide regardless of target
 *     process bitness, integrity, or container. Every key Windows sees
 *     passes through us.
 *   - Trade-off: ~25 us per event in the hook, well within the 300 ms
 *     timeout budget. SendInput to deliver the remapped keys is also
 *     in-process so there's no IPC cost.
 *
 * Re-entrancy:
 *   - SendInput stamps our session token in dwExtraInfo. On the next
 *     LL hook fire we recognise the token and pass through unchanged.
 *
 * The DLL approach still ships - it's kept as a secondary path. LL
 * hooks fire BEFORE per-process WH_KEYBOARD hooks, so if we suppress
 * here, the DLL hook never sees the event.
 */
#include <windows.h>
#include <string.h>

#include "mapping_defs.h"

extern void kv_shm_publish_verdict(uint32_t vkCode,
                                    uint32_t timestampMs,
                                    uint32_t flags,
                                    uint32_t extraInfoLow);
extern KVSharedBlock *kv_shm_view(void);

static HHOOK     g_llHook    = NULL;
static HANDLE    g_thread    = NULL;
static DWORD     g_threadId  = 0;
static HINSTANCE g_inst      = NULL;
static volatile LONG g_evtCount = 0;

/* Local cache of the mapping table. Refreshed lazily when the
 * shared-memory sequence counter changes. */
static KVSharedBlock g_local;
static LONG          g_localSeq = -1;
static BYTE          g_heldKeys[256];

static void refresh_local(const KVSharedBlock *sh) {
    LONG seq = sh->sequence;
    memcpy(&g_local, (const void*)sh, sizeof(KVSharedBlock));
    g_localSeq = seq;
}

/* Modifier wildcard matching.
 *
 * When the user types "ALT" in the trigger field it parses to VK_MENU
 * (0x12, generic Alt). But the LL hook only ever sees VK_LMENU (0xA4)
 * or VK_RMENU (0xA5); the generic VK is never delivered at the LL
 * layer. So a rule with VK_MENU must match EITHER LMENU or RMENU.
 * Same idea for VK_CONTROL and VK_SHIFT.
 */
static int vk_matches(BYTE rule_vk, BYTE event_vk) {
    if (rule_vk == event_vk) return 1;
    if (rule_vk == VK_MENU    && (event_vk == VK_LMENU    || event_vk == VK_RMENU))    return 1;
    if (rule_vk == VK_CONTROL && (event_vk == VK_LCONTROL || event_vk == VK_RCONTROL)) return 1;
    if (rule_vk == VK_SHIFT   && (event_vk == VK_LSHIFT   || event_vk == VK_RSHIFT))   return 1;
    return 0;
}

static int vk_is_held(BYTE rule_vk) {
    if (g_heldKeys[rule_vk]) return 1;
    if (rule_vk == VK_MENU)    return g_heldKeys[VK_LMENU]    || g_heldKeys[VK_RMENU];
    if (rule_vk == VK_CONTROL) return g_heldKeys[VK_LCONTROL] || g_heldKeys[VK_RCONTROL];
    if (rule_vk == VK_SHIFT)   return g_heldKeys[VK_LSHIFT]   || g_heldKeys[VK_RSHIFT];
    return 0;
}

/* Build INPUT events and SendInput, stamped with our session token so
 * we can identify our own re-entry. */
static void fire_remap(const KVMapping *m, const KVSharedBlock *sh) {
    if (!m || m->actionCount <= 0) return;

    ULONG_PTR extra = kv_session_extrainfo(sh->tokenPid, sh->tokenLow);

    INPUT inputs[KV_MAX_ACTION_KEYS * 2];
    ZeroMemory(inputs, sizeof(inputs));
    int idx = 0;
    int32_t at = m->actionType;
    int32_t n  = m->actionCount;
    if (n > KV_MAX_ACTION_KEYS) n = KV_MAX_ACTION_KEYS;

    if (at == KV_ACTION_PRESS_RELEASE || at == KV_ACTION_PRESS) {
        for (int32_t i = 0; i < n; i++) {
            inputs[idx].type           = INPUT_KEYBOARD;
            inputs[idx].ki.wVk         = m->actionKeys[i];
            inputs[idx].ki.dwExtraInfo = extra;
            idx++;
        }
    }
    if (at == KV_ACTION_PRESS_RELEASE || at == KV_ACTION_RELEASE) {
        for (int32_t i = n - 1; i >= 0; i--) {
            inputs[idx].type           = INPUT_KEYBOARD;
            inputs[idx].ki.wVk         = m->actionKeys[i];
            inputs[idx].ki.dwFlags     = KEYEVENTF_KEYUP;
            inputs[idx].ki.dwExtraInfo = extra;
            idx++;
        }
    }
    if (idx > 0) SendInput((UINT)idx, inputs, sizeof(INPUT));
}

static LRESULT CALLBACK ll_classify_proc(int code, WPARAM wParam, LPARAM lParam) {
    if (code != HC_ACTION) {
        return CallNextHookEx(NULL, code, wParam, lParam);
    }
    KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT *)lParam;
    if (!kb) return CallNextHookEx(NULL, code, wParam, lParam);

    KVSharedBlock *sh = kv_shm_view();
    if (!sh) return CallNextHookEx(NULL, code, wParam, lParam);

    BOOL keyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
    BYTE vk    = (BYTE)kb->vkCode;
    uint32_t extraLow = (uint32_t)(kb->dwExtraInfo & 0xFFFFFFFFu);
    BOOL isOurs = (extraLow == sh->tokenLow && sh->tokenLow != 0);

    /* Always publish the verdict for any other observer (DLL hook in
     * each process can read this if it still runs). */
    uint32_t ts = (uint32_t)kb->time;
    if (ts == 0) ts = (uint32_t)GetTickCount();
    kv_shm_publish_verdict((uint32_t)vk, ts, (uint32_t)kb->flags, extraLow);
    InterlockedIncrement(&g_evtCount);

    /* Our own injected event - pass through, never remap. */
    if (isOurs) return CallNextHookEx(NULL, code, wParam, lParam);

    /* Update held-key set BEFORE matching so the rule sees the right
     * state. (The set is for trigger combos with multiple keys.) */
    g_heldKeys[vk] = keyUp ? 0 : 1;

    /* Lazy refresh of the rule table. */
    if (sh->sequence != g_localSeq) {
        refresh_local(sh);
    }

    /* Linear rule scan. */
    BOOL isInjected = (kb->flags & LLKHF_INJECTED) ? TRUE : FALSE;
    for (int32_t i = 0; i < g_local.count && i < KV_MAX_MAPPINGS; i++) {
        const KVMapping *m = &g_local.mappings[i];
        if (!m->enabled) continue;

        if (m->sourceFilter == KV_SOURCE_LOCAL  && isInjected) continue;
        if (m->sourceFilter == KV_SOURCE_REMOTE && !isInjected) continue;

        BOOL wantUp = (m->dispatch == KV_DISPATCH_RELEASED);
        if (wantUp != keyUp) continue;

        /* Changed key must match a trigger key (with modifier wildcard). */
        BOOL changedIsTrigger = FALSE;
        for (int32_t j = 0; j < m->triggerCount; j++) {
            if (vk_matches(m->triggerKeys[j], vk)) {
                changedIsTrigger = TRUE; break;
            }
        }
        if (!changedIsTrigger) continue;

        /* All other trigger keys must be currently held (wildcard-aware). */
        BOOL match = TRUE;
        for (int32_t j = 0; j < m->triggerCount; j++) {
            BYTE tk = m->triggerKeys[j];
            if (wantUp && vk_matches(tk, vk)) continue;   /* the just-released key */
            if (!vk_is_held(tk)) { match = FALSE; break; }
        }
        if (!match) continue;

        /* Match! Suppress original and synthesise action. */
        fire_remap(m, sh);
        return 1;
    }

    return CallNextHookEx(NULL, code, wParam, lParam);
}

static DWORD WINAPI classifier_thread_proc(LPVOID arg) {
    HANDLE readyEvent = (HANDLE)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    g_llHook = SetWindowsHookExW(WH_KEYBOARD_LL, ll_classify_proc, g_inst, 0);

    MSG dummy;
    PeekMessageW(&dummy, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    if (readyEvent) SetEvent(readyEvent);

    if (!g_llHook) return 1;

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(g_llHook);
    g_llHook = NULL;
    return 0;
}

BOOL kv_classifier_start(HINSTANCE inst) {
    if (g_thread) return TRUE;
    g_inst = inst;
    memset(g_heldKeys, 0, sizeof(g_heldKeys));

    HANDLE ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_thread = CreateThread(NULL, 0, classifier_thread_proc, ready, 0, &g_threadId);
    if (!g_thread) {
        if (ready) CloseHandle(ready);
        return FALSE;
    }
    WaitForSingleObject(ready, 2000);
    CloseHandle(ready);
    return (g_llHook != NULL);
}

void kv_classifier_stop(void) {
    if (!g_thread) return;
    PostThreadMessageA(g_threadId, WM_QUIT, 0, 0);
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = NULL;
    g_threadId = 0;
}

LONG kv_classifier_event_count(void) { return g_evtCount; }
