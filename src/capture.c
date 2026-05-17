/*
 * capture.c - capture-set state management.
 *
 * v0.10.2: dropped the LL hook AND the polling thread.
 *
 * Lesson from real-world testing: both WH_KEYBOARD_LL and
 * GetAsyncKeyState polling have session/focus quirks that make them
 * unreliable in some setups (RDP, virtualized environments, OEM
 * power-management hooks, etc.). What ALWAYS works: keystrokes reach
 * the focused window's WindowProc. So during capture mode the main
 * window force-takes focus and intercepts WM_KEYDOWN / WM_KEYUP
 * directly, calling kv_capture_add / kv_capture_remove from there.
 *
 * This file now only owns the held-set and counted-set state, which
 * the main UI thread mutates synchronously. No threads, no hooks.
 */
#include <windows.h>
#include <string.h>

#include "mapping_defs.h"

static BYTE  g_capturedKeys[KV_MAX_TRIGGER_KEYS];
static int   g_capturedCount = 0;
static BYTE  g_heldDuringCapture[256];
static int   g_heldNow = 0;

void kv_capture_reset(void) {
    memset(g_heldDuringCapture, 0, sizeof(g_heldDuringCapture));
    memset(g_capturedKeys, 0, sizeof(g_capturedKeys));
    g_capturedCount = 0;
    g_heldNow = 0;
}

/* Returns 1 if the key was newly added, 0 if already held. */
int kv_capture_add(BYTE vk) {
    if (g_heldDuringCapture[vk]) return 0;
    g_heldDuringCapture[vk] = 1;
    g_heldNow++;

    if (g_capturedCount < KV_MAX_TRIGGER_KEYS) {
        for (int i = 0; i < g_capturedCount; i++) {
            if (g_capturedKeys[i] == vk) return 1;
        }
        g_capturedKeys[g_capturedCount++] = vk;
    }
    return 1;
}

/* Returns 1 if the released key emptied the held set AND we had captured
 * keys (i.e., capture is now complete). */
int kv_capture_remove(BYTE vk) {
    if (!g_heldDuringCapture[vk]) return 0;
    g_heldDuringCapture[vk] = 0;
    if (g_heldNow > 0) g_heldNow--;
    return (g_heldNow == 0 && g_capturedCount > 0) ? 1 : 0;
}

int kv_capture_result(BYTE *outKeys, int max) {
    int n = g_capturedCount;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) outKeys[i] = g_capturedKeys[i];
    return n;
}

int kv_capture_held_count(void) { return g_heldNow; }
int kv_capture_count(void)      { return g_capturedCount; }
