/*
 * inject.c - SendInput helpers for the hook DLL.
 *
 * v0.10:
 *   - dwExtraInfo is built from the live session token in shared
 *     memory. The classifier sees the same value on its LL hook side,
 *     publishes it into the verdict ring, and the per-process DLL
 *     uses it to recognise our own events.
 *   - g_dispatching is also flipped around SendInput as belt-and-
 *     braces protection for the auto-repeat collision case where the
 *     verdict slot for our vk could be overwritten between LL fire
 *     and WH_KEYBOARD dispatch on the same thread.
 */
#include <windows.h>

#include "mapping_defs.h"

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define KV_TLS _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#  define KV_TLS __thread
#else
#  define KV_TLS __declspec(thread)
#endif

extern KV_TLS int g_dispatching;
extern const KVSharedBlock *kv_dll_shared(void);

void kv_fire_action(const KVMapping *m) {
    if (!m) return;
    if (m->actionCount <= 0) return;

    const KVSharedBlock *sh = kv_dll_shared();
    ULONG_PTR extra = sh ? kv_session_extrainfo(sh->tokenPid, sh->tokenLow)
                          : (ULONG_PTR)0;

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

    if (idx > 0) {
        g_dispatching = 1;
        SendInput((UINT)idx, inputs, sizeof(INPUT));
        g_dispatching = 0;
    }
}
