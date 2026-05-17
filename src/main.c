/*
 * main.c - kvmapper.exe entry, main window, tray, mapping editor.
 *
 * Mirrors the layout of scroll_toggle.c referenced in plan.md:
 *   - WinMain handles cmdline modes (/tray, /stop), creates the
 *     single-instance mutex, owns the shared memory.
 *   - Main window hosts a listbox of mappings + a Create/Edit panel
 *     that drives the capture state machine.
 *   - NOTIFYICONDATA tray icon with right-click context menu.
 *
 * Threading model:
 *   - UI thread runs the message loop and owns all HWNDs.
 *   - Capture thread (capture.c) runs the WH_KEYBOARD_LL hook and
 *     PostThreadMessages WM_APP_CAPTURE_DONE back to the UI thread.
 *   - Hook DLL runs inside every other GUI process; the exe never
 *     touches it after LoadLibrary + install_hook.
 */
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>

#include "mapping_defs.h"
#include "icon_data.h"

/* Auto-link hints for MSVC builds. mingw/zig need explicit -l flags
 * on the command line - the build scripts pass them. */
#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#endif

/* ---------- Forward declarations from other compilation units ---------- */
extern KVSharedBlock *kv_shm_create(void);
extern void           kv_shm_destroy(void);
extern void           kv_shm_publish(void);
extern KVSharedBlock *kv_shm_view(void);

extern void kv_config_load(const char *path, KVSharedBlock *out);
extern int  kv_config_save(const char *path, const KVSharedBlock *src);
extern void kv_format_mapping_label(const KVMapping *m, char *out, size_t cap);
extern const char *kv_name_from_vk(BYTE vk);
extern int kv_parse_vk_list(const char *src, BYTE *out, int max);

/* Capture-set state. Mutated synchronously from MainWndProc during
 * WM_KEYDOWN/WM_KEYUP in ED_RECORDING_* states. */
extern void kv_capture_reset(void);
extern int  kv_capture_add(BYTE vk);
extern int  kv_capture_remove(BYTE vk);
extern int  kv_capture_result(BYTE *outKeys, int max);
extern int  kv_capture_held_count(void);
extern int  kv_capture_count(void);

/* v0.10 classifier (src/classifier.c). Always-on LL hook on a dedicated
 * high-priority thread; publishes verdicts into KVSharedBlock.ring. */
extern BOOL kv_classifier_start(HINSTANCE inst);
extern void kv_classifier_stop(void);
extern LONG kv_classifier_event_count(void);

/* ---------- Constants ---------- */
#define KV_WND_CLASS        "KVMapperMainWnd_v1"
#define KV_MUTEX_NAME       "Global\\KVMapperInstanceMutex_v1"
#define KV_STOP_EVENT_NAME  "Global\\KVMapperStopEvent_v1"
#define KV_CFG_FILENAME     "kvmapper_mappings.txt"

#define WM_TRAY_ICON        (WM_APP + 100)
#define WM_APP_CAPTURE_DONE (WM_APP + 1)
#define WM_APP_STOP_REQUEST (WM_APP + 2)
#define WM_APP_CAPTURE_TICK (WM_APP + 3)   /* live key-by-key feedback during capture */

#define ID_TRAY_FIRST       2000
#define IDM_TRAY_SHOW       2001
#define IDM_TRAY_ABOUT      2002
#define IDM_TRAY_GITHUB     2003
#define IDM_TRAY_EXIT       2004

/* Control IDs */
#define IDC_LIST_MAPPINGS   100
#define IDC_BTN_CREATE      101
#define IDC_BTN_EDIT        102
#define IDC_BTN_DELETE      103
#define IDC_BTN_ABOUT       104
#define IDC_BTN_HIDE        105
#define IDC_STATIC_STATUS   106
#define IDC_STATIC_TITLE    107

/* Editor panel controls */
#define IDC_EDITOR_PANEL    200
#define IDC_BTN_REC_TRIG    201
#define IDC_BTN_REC_ACT     202
#define IDC_EDIT_TRIGGER    203
#define IDC_EDIT_ACTION     204
#define IDC_EDIT_LABEL      205
#define IDC_RADIO_PRESSED   206
#define IDC_RADIO_RELEASED  207
#define IDC_RADIO_PR        208
#define IDC_RADIO_P         209
#define IDC_RADIO_R         210
#define IDC_BTN_SAVE        211
#define IDC_BTN_CANCEL      212

/* v0.9 release: test-area edit. Lets the user verify mappings fire
 * without alt-tabbing to Notepad. Multi-line so they can type freely. */
#define IDC_EDIT_TEST       213
#define IDC_BTN_TEST_CLEAR  214

/* ---------- Globals ---------- */
static HINSTANCE      g_inst         = NULL;
static HWND           g_main         = NULL;
static HWND           g_editorPanel  = NULL;
static HMODULE        g_hookDll      = NULL;
static BOOL           g_hookActive   = FALSE;
static KVSharedBlock *g_shared       = NULL;
static HFONT          g_font         = NULL;
static HFONT          g_titleFont    = NULL;
static NOTIFYICONDATAA g_nid         = {0};
static HANDLE         g_instMutex    = NULL;
static HANDLE         g_stopEvent    = NULL;
static HANDLE         g_stopThread   = NULL;
static char           g_cfgPath[MAX_PATH] = {0};

/* Forward decls for editor helpers (defined further down). */
static void kv_show_editor_controls(BOOL show);

/* Editor state */
typedef enum {
    ED_HIDDEN = 0,
    ED_RECORDING_TRIGGER,
    ED_TRIGGER_DONE,
    ED_RECORDING_ACTION,
    ED_ACTION_DONE
} EditorState;

static EditorState g_edState  = ED_HIDDEN;
static int         g_editIdx  = -1;        /* >=0 = edit existing, -1 = new */
static KVMapping   g_edMapping;
static int         g_capturingTrigger = 0; /* 1 = trigger, 0 = action */

/* DLL exports */
typedef BOOL (*pfn_install_hook)(void);
typedef BOOL (*pfn_uninstall_hook)(void);
typedef BOOL (*pfn_reload_mappings)(void);
static pfn_install_hook    p_install   = NULL;
static pfn_uninstall_hook  p_uninstall = NULL;
static pfn_reload_mappings p_reload    = NULL;

/* ---------- Helpers ---------- */
static void kv_resolve_cfg_path(void) {
    GetModuleFileNameA(NULL, g_cfgPath, MAX_PATH);
    char *p = strrchr(g_cfgPath, '\\');
    if (p) {
        p[1] = 0;
        strncat(g_cfgPath, KV_CFG_FILENAME, MAX_PATH - strlen(g_cfgPath) - 1);
    } else {
        strncpy(g_cfgPath, KV_CFG_FILENAME, MAX_PATH - 1);
    }
}

static void kv_set_status(const char *s) {
    HWND h = GetDlgItem(g_main, IDC_STATIC_STATUS);
    if (h) SetWindowTextA(h, s);
}

static void kv_format_keys(const BYTE *keys, int n, char *out, size_t cap) {
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        const char *nm = kv_name_from_vk(keys[i]);
        char tmp[8];
        if (!nm) { snprintf(tmp, sizeof(tmp), "0x%02X", keys[i]); nm = tmp; }
        if (i > 0) strncat(out, "+", cap - strlen(out) - 1);
        strncat(out, nm, cap - strlen(out) - 1);
    }
}

/* ---------- DLL loading ---------- */
/* Resolve and load the hook DLL. We try in order:
 *   1. The canonical name (kvmapper_hook.dll or kvmapper_hook_x86.dll)
 *      next to the exe.
 *   2. The canonical name via the system PATH.
 *   3. FindFirstFile pattern next to the exe - any file matching
 *      kvmapper_hook*.dll (for x64) or kvmapper_hook_x86*.dll (for x86)
 *      so versioned drops like kvmapper_hook_v11.dll just work without
 *      requiring a rename.
 */
static BOOL kv_resolve_dll_imports(void) {
    p_install   = (pfn_install_hook)   (void*)GetProcAddress(g_hookDll, "install_hook");
    p_uninstall = (pfn_uninstall_hook) (void*)GetProcAddress(g_hookDll, "uninstall_hook");
    p_reload    = (pfn_reload_mappings)(void*)GetProcAddress(g_hookDll, "reload_mappings");
    return (p_install && p_uninstall && p_reload);
}

static BOOL kv_load_hook_dll(void) {
#if defined(_WIN64)
    const char *dllname = "kvmapper_hook.dll";
    const char *pattern = "kvmapper_hook*.dll";
#else
    const char *dllname = "kvmapper_hook_x86.dll";
    const char *pattern = "kvmapper_hook_x86*.dll";
#endif

    char exedir[MAX_PATH];
    GetModuleFileNameA(NULL, exedir, MAX_PATH);
    char *bs = strrchr(exedir, '\\');
    if (bs) bs[1] = 0;

    /* 1. Canonical name next to the exe */
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s%s", exedir, dllname);
    g_hookDll = LoadLibraryA(path);
    if (g_hookDll && kv_resolve_dll_imports()) return TRUE;
    if (g_hookDll) { FreeLibrary(g_hookDll); g_hookDll = NULL; }

    /* 2. Canonical name via PATH */
    g_hookDll = LoadLibraryA(dllname);
    if (g_hookDll && kv_resolve_dll_imports()) return TRUE;
    if (g_hookDll) { FreeLibrary(g_hookDll); g_hookDll = NULL; }

    /* 3. Glob next to the exe - pick any matching versioned DLL */
    char globpath[MAX_PATH];
    snprintf(globpath, sizeof(globpath), "%s%s", exedir, pattern);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(globpath, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            /* For x64: don't accidentally load the _x86 variant. */
#if defined(_WIN64)
            if (strstr(fd.cFileName, "_x86")) continue;
#endif
            char fullpath[MAX_PATH];
            snprintf(fullpath, sizeof(fullpath), "%s%s", exedir, fd.cFileName);
            g_hookDll = LoadLibraryA(fullpath);
            if (g_hookDll && kv_resolve_dll_imports()) {
                FindClose(h);
                return TRUE;
            }
            if (g_hookDll) { FreeLibrary(g_hookDll); g_hookDll = NULL; }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    return FALSE;
}

static void kv_hook_start(void) {
    if (g_hookActive) return;
    if (!p_install) return;
    if (p_install()) {
        g_hookActive = TRUE;
        kv_set_status("Hook: ACTIVE");
    } else {
        kv_set_status("Hook: FAILED to install");
    }
}

static void kv_hook_stop(void) {
    if (!g_hookActive) return;
    if (p_uninstall) p_uninstall();
    g_hookActive = FALSE;
}

/* ---------- Mapping list UI ---------- */
static void kv_list_rebuild(void) {
    HWND lb = GetDlgItem(g_main, IDC_LIST_MAPPINGS);
    if (!lb) return;
    SendMessageA(lb, LB_RESETCONTENT, 0, 0);

    char buf[256];
    for (int32_t i = 0; i < g_shared->count; i++) {
        char tag[8];
        snprintf(tag, sizeof(tag), "[%s] ", g_shared->mappings[i].enabled ? "x" : " ");
        char body[200];
        kv_format_mapping_label(&g_shared->mappings[i], body, sizeof(body));
        snprintf(buf, sizeof(buf), "%s%s", tag, body);
        SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)buf);
    }
}

static void kv_persist_and_publish(void) {
    kv_shm_publish();
    if (p_reload) p_reload();
    if (!kv_config_save(g_cfgPath, g_shared)) {
        char err[MAX_PATH + 512];
        snprintf(err, sizeof(err),
            "Failed to save mappings to:\n  %s\n\n"
            "Your mapping is active for this session but will be lost on exit.\n\n"
            "Common causes:\n"
            "  - The folder is read-only\n"
            "  - The exe is in a UAC-protected path (Program Files)\n"
            "  - Antivirus is blocking writes\n"
            "  - The file is open in another program\n\n"
            "Fix: move kvmapper.exe to a writable folder (Documents, Desktop, "
            "or a custom Tools folder under your user profile) and try again.",
            g_cfgPath);
        MessageBoxA(g_main, err, "KVMapper - Save failed",
                    MB_OK | MB_ICONWARNING);
        kv_set_status("Save FAILED - mapping not persisted");
    } else {
        char st[MAX_PATH + 32];
        snprintf(st, sizeof(st), "Saved to %s", g_cfgPath);
        kv_set_status(st);
    }
    kv_list_rebuild();
}

/* ---------- Editor panel ---------- */
static void kv_editor_show(int editIdx) {
    g_editIdx = editIdx;
    if (editIdx >= 0 && editIdx < g_shared->count) {
        g_edMapping = g_shared->mappings[editIdx];
    } else {
        memset(&g_edMapping, 0, sizeof(g_edMapping));
        g_edMapping.enabled    = 1;
        g_edMapping.dispatch   = KV_DISPATCH_PRESSED;
        g_edMapping.actionType = KV_ACTION_PRESS_RELEASE;
    }
    g_edState = ED_TRIGGER_DONE;   /* enter at the "ready to record" state */

    /* Update editor fields */
    char buf[128];
    kv_format_keys(g_edMapping.triggerKeys, g_edMapping.triggerCount, buf, sizeof(buf));
    SetDlgItemTextA(g_main, IDC_EDIT_TRIGGER, buf);
    kv_format_keys(g_edMapping.actionKeys, g_edMapping.actionCount, buf, sizeof(buf));
    SetDlgItemTextA(g_main, IDC_EDIT_ACTION, buf);
    SetDlgItemTextA(g_main, IDC_EDIT_LABEL, g_edMapping.label);

    CheckRadioButton(g_main, IDC_RADIO_PRESSED, IDC_RADIO_RELEASED,
        g_edMapping.dispatch == KV_DISPATCH_RELEASED ? IDC_RADIO_RELEASED : IDC_RADIO_PRESSED);
    int r = IDC_RADIO_PR;
    if (g_edMapping.actionType == KV_ACTION_PRESS)   r = IDC_RADIO_P;
    if (g_edMapping.actionType == KV_ACTION_RELEASE) r = IDC_RADIO_R;
    CheckRadioButton(g_main, IDC_RADIO_PR, IDC_RADIO_R, r);

    ShowWindow(g_editorPanel, SW_SHOW);
    /* Focus the trigger field so user can type or click Record. */
    HWND te = GetDlgItem(g_main, IDC_EDIT_TRIGGER);
    if (te) {
        SetFocus(te);
        SendMessageA(te, EM_SETSEL, 0, -1);
    }
}

static void kv_editor_hide(void) {
    /* Always clear capture state so we don't leak it into the next session. */
    kv_capture_reset();
    g_edState = ED_HIDDEN;
    g_editIdx = -1;
    ShowWindow(g_editorPanel, SW_HIDE);
}

static void kv_editor_start_capture(int trigger) {
    g_capturingTrigger = trigger;
    kv_capture_reset();
    if (trigger) {
        SetDlgItemTextA(g_main, IDC_EDIT_TRIGGER, "[listening] press your combo (release all when done)");
        g_edState = ED_RECORDING_TRIGGER;
    } else {
        SetDlgItemTextA(g_main, IDC_EDIT_ACTION, "[listening] press your combo (release all when done)");
        g_edState = ED_RECORDING_ACTION;
    }
    /* Force focus to the main window so WM_KEYDOWN reaches MainWndProc.
     * Without this, focus is on the Record button and DefWindowProc beeps. */
    SetFocus(g_main);
    kv_set_status(g_capturingTrigger
        ? "Recording trigger: press your keys, then release all"
        : "Recording action: press your keys, then release all");
}

static void kv_editor_on_capture_done(void) {
    BYTE keys[KV_MAX_TRIGGER_KEYS];
    int n = kv_capture_result(keys, KV_MAX_TRIGGER_KEYS);

    char buf[128];
    kv_format_keys(keys, n, buf, sizeof(buf));

    if (g_capturingTrigger) {
        for (int i = 0; i < n; i++) g_edMapping.triggerKeys[i] = keys[i];
        g_edMapping.triggerCount = n;
        SetDlgItemTextA(g_main, IDC_EDIT_TRIGGER, buf);
        g_edState = ED_TRIGGER_DONE;
    } else {
        for (int i = 0; i < n; i++) g_edMapping.actionKeys[i] = keys[i];
        g_edMapping.actionCount = n;
        SetDlgItemTextA(g_main, IDC_EDIT_ACTION, buf);
        g_edState = ED_ACTION_DONE;
    }
}

static void kv_editor_save(void) {
    /* Pull radio choices */
    if (IsDlgButtonChecked(g_main, IDC_RADIO_RELEASED) == BST_CHECKED)
        g_edMapping.dispatch = KV_DISPATCH_RELEASED;
    else
        g_edMapping.dispatch = KV_DISPATCH_PRESSED;

    if (IsDlgButtonChecked(g_main, IDC_RADIO_P) == BST_CHECKED)
        g_edMapping.actionType = KV_ACTION_PRESS;
    else if (IsDlgButtonChecked(g_main, IDC_RADIO_R) == BST_CHECKED)
        g_edMapping.actionType = KV_ACTION_RELEASE;
    else
        g_edMapping.actionType = KV_ACTION_PRESS_RELEASE;

    GetDlgItemTextA(g_main, IDC_EDIT_LABEL, g_edMapping.label, sizeof(g_edMapping.label));

    /* Re-parse the trigger and action edit fields. If the user typed
     * names manually (e.g. "HANGUL"), the typed text takes precedence
     * over whatever was recorded. Skip "[listening] ..." placeholder. */
    char tbuf[128], abuf[128];
    GetDlgItemTextA(g_main, IDC_EDIT_TRIGGER, tbuf, sizeof(tbuf));
    GetDlgItemTextA(g_main, IDC_EDIT_ACTION,  abuf, sizeof(abuf));
    if (tbuf[0] && tbuf[0] != '[') {
        BYTE parsed[KV_MAX_TRIGGER_KEYS] = {0};
        int n = kv_parse_vk_list(tbuf, parsed, KV_MAX_TRIGGER_KEYS);
        if (n > 0) {
            memset(g_edMapping.triggerKeys, 0, sizeof(g_edMapping.triggerKeys));
            for (int i = 0; i < n; i++) g_edMapping.triggerKeys[i] = parsed[i];
            g_edMapping.triggerCount = n;
        }
    }
    if (abuf[0] && abuf[0] != '[') {
        BYTE parsed[KV_MAX_ACTION_KEYS] = {0};
        int n = kv_parse_vk_list(abuf, parsed, KV_MAX_ACTION_KEYS);
        if (n > 0) {
            memset(g_edMapping.actionKeys, 0, sizeof(g_edMapping.actionKeys));
            for (int i = 0; i < n; i++) g_edMapping.actionKeys[i] = parsed[i];
            g_edMapping.actionCount = n;
        }
    }

    if (g_edMapping.triggerCount == 0 || g_edMapping.actionCount == 0) {
        /* Tell the user exactly which field is empty and what we tried
         * to parse, so they can fix it. The most common case: an
         * unrecognised name (e.g. "HANGUL" after we removed aliases). */
        char trigText[128] = {0}, actText[128] = {0};
        GetDlgItemTextA(g_main, IDC_EDIT_TRIGGER, trigText, sizeof(trigText));
        GetDlgItemTextA(g_main, IDC_EDIT_ACTION,  actText,  sizeof(actText));

        char msg[800];
        const char *which = "both trigger and action";
        if (g_edMapping.triggerCount == 0 && g_edMapping.actionCount > 0) which = "TRIGGER";
        if (g_edMapping.actionCount  == 0 && g_edMapping.triggerCount > 0) which = "ACTION";

        snprintf(msg, sizeof(msg),
            "Cannot save - %s could not be parsed into any keys.\n\n"
            "Trigger field text: \"%s\"\n"
            "Action  field text: \"%s\"\n\n"
            "Use VK names (see README for full list). Common ones:\n"
            "  ALT / LALT / RALT       CTRL / LCTRL / RCTRL\n"
            "  SHIFT / LSHIFT / RSHIFT WIN / LWIN / RWIN\n"
            "  F1..F24, A-Z, 0-9, ENTER, ESC, TAB, SPACE\n"
            "  HOME, END, INS, DEL, PGUP, PGDN\n"
            "  HANGUL, KANA, HANJA, KANJI, IME_ON, IME_OFF\n"
            "  VOLUME_UP/DOWN/MUTE, MEDIA_PLAY_PAUSE, BROWSER_BACK\n\n"
            "Combine with '+' (e.g. CTRL+SHIFT+A).\n"
            "Or use raw hex: 0x15 (HANGUL/KANA), 0x19 (HANJA/KANJI).",
            which, trigText, actText);
        MessageBoxA(g_main, msg, "KVMapper - parse error", MB_OK | MB_ICONWARNING);
        return;
    }

    if (g_editIdx >= 0 && g_editIdx < g_shared->count) {
        g_shared->mappings[g_editIdx] = g_edMapping;
    } else if (g_shared->count < KV_MAX_MAPPINGS) {
        g_shared->mappings[g_shared->count++] = g_edMapping;
    } else {
        MessageBoxA(g_main, "Maximum mappings reached.", "KVMapper", MB_OK | MB_ICONWARNING);
        return;
    }

    kv_persist_and_publish();
    /* Use kv_close_editor (full hide) not kv_editor_hide (panel only),
     * because the editor controls are siblings of the panel, not
     * children, and need to be hidden individually. */
    kv_editor_hide();
    kv_show_editor_controls(FALSE);
}

/* ---------- Tray icon ---------- */
/* Parse the embedded ICO blob (icon_data.h) and create an HICON for a
 * specific size. Searches all entries for the closest match.
 *
 * ICO file layout:
 *   ICONDIR        6 bytes:  reserved(2) type(2) count(2)
 *   ICONDIRENTRY  16 bytes each: w(1) h(1) palette(1) res(1)
 *                                 planes(2) bpp(2) size(4) offset(4)
 *   then raw DIB or PNG bytes per entry at the given offsets.
 *
 * For each entry we score by closeness to (cx, cy). Pass 0,0 for any
 * size (we then bias toward 32x32 as a reasonable default).
 */
static HICON kv_icon_from_embedded(int cx, int cy) {
    if (kv_ico_size < 6) return NULL;
    WORD reserved, type, count;
    memcpy(&reserved, kv_ico_data + 0, 2);
    memcpy(&type,     kv_ico_data + 2, 2);
    memcpy(&count,    kv_ico_data + 4, 2);
    if (reserved != 0 || type != 1 || count == 0) return NULL;

    DWORD bestOff = 0, bestSize = 0;
    int bestScore = -1000000;
    int targetW = cx ? cx : 32;
    int targetH = cy ? cy : 32;

    for (WORD i = 0; i < count; i++) {
        size_t eo = 6 + (size_t)i * 16;
        if (eo + 16 > kv_ico_size) break;
        BYTE w = kv_ico_data[eo + 0];
        BYTE h = kv_ico_data[eo + 1];
        DWORD sz, off;
        memcpy(&sz,  kv_ico_data + eo + 8,  4);
        memcpy(&off, kv_ico_data + eo + 12, 4);
        int iw = w ? w : 256;
        int ih = h ? h : 256;
        /* Higher score = better. Exact match wins; otherwise smallest
         * absolute distance. */
        int score = -(abs(iw - targetW) + abs(ih - targetH));
        if (score > bestScore && sz > 0 && off + sz <= kv_ico_size) {
            bestScore = score;
            bestOff = off;
            bestSize = sz;
        }
    }
    if (bestSize == 0) return NULL;

    return CreateIconFromResourceEx(
        (PBYTE)(kv_ico_data + bestOff),
        (DWORD)bestSize,
        TRUE,               /* fIcon = TRUE */
        0x00030000,         /* version */
        cx, cy,             /* desired size; 0 means use resource's own */
        LR_DEFAULTCOLOR);
}

/* Load the right icon for a given context.
 *   size = 16 for tray, 32 for window/title bar, 0 for "let Windows decide".
 *
 * Priority order:
 *   1. Embedded ICO blob (most reliable - lives inside the exe).
 *   2. Embedded .rc resource id 1 (only if windres compiled the .rc).
 *   3. Default app icon.
 */
static HICON kv_load_tray_icon_size(int size) {
    HICON h = kv_icon_from_embedded(size, size);
    if (h) return h;

    h = LoadIconA(g_inst, MAKEINTRESOURCEA(1));
    if (h) return h;

    return LoadIconA(NULL, MAKEINTRESOURCEA(32512));
}

static HICON kv_load_tray_icon(void) {
    return kv_load_tray_icon_size(16);   /* tray defaults to 16x16 */
}

static void kv_tray_add(HWND hwnd) {
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY_ICON;
    g_nid.hIcon            = kv_load_tray_icon();
    strncpy(g_nid.szTip, "KVMapper - KVM Remote Hotkey Mapper", sizeof(g_nid.szTip) - 1);
    Shell_NotifyIconA(NIM_ADD, &g_nid);
}

static void kv_tray_remove(void) {
    Shell_NotifyIconA(NIM_DELETE, &g_nid);
}

static void kv_tray_menu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING | MF_GRAYED, 0,
        g_hookActive ? "Hook: Active" : "Hook: Inactive");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, IDM_TRAY_SHOW,   "Show Window");
    AppendMenuA(m, MF_STRING, IDM_TRAY_ABOUT,  "About...");
    AppendMenuA(m, MF_STRING, IDM_TRAY_GITHUB, "Open GitHub Page");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, IDM_TRAY_EXIT,   "Exit");

    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
}

/* ---------- Stop-event watcher thread ---------- */
static DWORD WINAPI stop_watcher_proc(LPVOID arg) {
    (void)arg;
    if (!g_stopEvent) return 0;
    WaitForSingleObject(g_stopEvent, INFINITE);
    PostMessageA(g_main, WM_APP_STOP_REQUEST, 0, 0);
    return 0;
}

/* ---------- Window proc ---------- */
static void kv_create_controls(HWND hwnd) {
    /* Title */
    CreateWindowA("STATIC", "KVMapper - KVM Remote Hotkey Mapper",
        WS_CHILD | WS_VISIBLE,
        12, 8, 460, 22, hwnd, (HMENU)IDC_STATIC_TITLE, g_inst, NULL);

    /* Listbox of mappings */
    CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS,
        12, 40, 460, 180, hwnd, (HMENU)IDC_LIST_MAPPINGS, g_inst, NULL);

    CreateWindowA("BUTTON", "+ Create Hotkey",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        12, 226, 130, 28, hwnd, (HMENU)IDC_BTN_CREATE, g_inst, NULL);
    CreateWindowA("BUTTON", "Edit",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        150, 226, 70, 28, hwnd, (HMENU)IDC_BTN_EDIT, g_inst, NULL);
    CreateWindowA("BUTTON", "Delete",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        226, 226, 80, 28, hwnd, (HMENU)IDC_BTN_DELETE, g_inst, NULL);

    /* Status line */
    CreateWindowA("STATIC", "Hook: starting...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        12, 262, 460, 20, hwnd, (HMENU)IDC_STATIC_STATUS, g_inst, NULL);

    /* Test area - focus this and type to verify hotkey mappings work
     * without alt-tabbing to Notepad. The LL hook in classifier.c sees
     * the keys and fires any matching rule; SendInput delivers the
     * remapped keys back here. */
    CreateWindowA("STATIC", "Test area (click here, then type to verify mappings):",
        WS_CHILD | WS_VISIBLE, 12, 600, 360, 18, hwnd, NULL, g_inst, NULL);
    CreateWindowA("BUTTON", "Clear",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        388, 596, 80, 22, hwnd, (HMENU)IDC_BTN_TEST_CLEAR, g_inst, NULL);
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
        12, 620, 460, 70, hwnd, (HMENU)IDC_EDIT_TEST, g_inst, NULL);

    /* About / Hide buttons */
    CreateWindowA("BUTTON", "About",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        12, 698, 80, 28, hwnd, (HMENU)IDC_BTN_ABOUT, g_inst, NULL);
    CreateWindowA("BUTTON", "Hide to Tray",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        372, 698, 100, 28, hwnd, (HMENU)IDC_BTN_HIDE, g_inst, NULL);

    /* Editor panel (initially hidden). Plain STATIC, no SS_GRAYFRAME -
     * SS_GRAYFRAME draws a grey filled frame that visually subdues every
     * EDIT/BUTTON layered over it, which users mistake for "the controls
     * are disabled". Use a transparent ETCHED frame instead. */
    g_editorPanel = CreateWindowExA(WS_EX_CONTROLPARENT,
        "STATIC", "",
        WS_CHILD | SS_ETCHEDFRAME,
        12, 290, 460, 270, hwnd, (HMENU)IDC_EDITOR_PANEL, g_inst, NULL);

    /* Editor controls live as siblings of the panel for simpler painting */
    CreateWindowA("STATIC", "Trigger keys:",
        WS_CHILD | WS_VISIBLE, 24, 300, 100, 18, hwnd, NULL, g_inst, NULL);
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        24, 320, 320, 22, hwnd, (HMENU)IDC_EDIT_TRIGGER, g_inst, NULL);
    CreateWindowA("BUTTON", "Record Trigger",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        352, 320, 116, 22, hwnd, (HMENU)IDC_BTN_REC_TRIG, g_inst, NULL);

    CreateWindowA("STATIC", "Dispatch when:",
        WS_CHILD | WS_VISIBLE, 24, 350, 100, 18, hwnd, NULL, g_inst, NULL);
    CreateWindowA("BUTTON", "Pressed",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
        130, 350, 90, 18, hwnd, (HMENU)IDC_RADIO_PRESSED, g_inst, NULL);
    CreateWindowA("BUTTON", "Released",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        220, 350, 90, 18, hwnd, (HMENU)IDC_RADIO_RELEASED, g_inst, NULL);

    CreateWindowA("STATIC", "Action type:",
        WS_CHILD | WS_VISIBLE, 24, 374, 100, 18, hwnd, NULL, g_inst, NULL);
    CreateWindowA("BUTTON", "Press+Release",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
        130, 374, 120, 18, hwnd, (HMENU)IDC_RADIO_PR, g_inst, NULL);
    CreateWindowA("BUTTON", "Press only",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        252, 374, 100, 18, hwnd, (HMENU)IDC_RADIO_P, g_inst, NULL);
    CreateWindowA("BUTTON", "Release only",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        354, 374, 110, 18, hwnd, (HMENU)IDC_RADIO_R, g_inst, NULL);

    CreateWindowA("STATIC", "Action keys:",
        WS_CHILD | WS_VISIBLE, 24, 398, 100, 18, hwnd, NULL, g_inst, NULL);
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        24, 418, 320, 22, hwnd, (HMENU)IDC_EDIT_ACTION, g_inst, NULL);
    CreateWindowA("BUTTON", "Record Action",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        352, 418, 116, 22, hwnd, (HMENU)IDC_BTN_REC_ACT, g_inst, NULL);

    CreateWindowA("STATIC", "Label (optional):",
        WS_CHILD | WS_VISIBLE, 24, 448, 110, 18, hwnd, NULL, g_inst, NULL);
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        135, 446, 333, 22, hwnd, (HMENU)IDC_EDIT_LABEL, g_inst, NULL);

    CreateWindowA("BUTTON", "Save",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        388, 524, 80, 28, hwnd, (HMENU)IDC_BTN_SAVE, g_inst, NULL);
    CreateWindowA("BUTTON", "Cancel",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        302, 524, 80, 28, hwnd, (HMENU)IDC_BTN_CANCEL, g_inst, NULL);

    /* Hide editor controls initially - they belong to the panel state. */
    static const int editor_ids[] = {
        IDC_EDIT_TRIGGER, IDC_BTN_REC_TRIG,
        IDC_EDIT_ACTION,  IDC_BTN_REC_ACT,
        IDC_EDIT_LABEL,
        IDC_RADIO_PRESSED, IDC_RADIO_RELEASED,
        IDC_RADIO_PR, IDC_RADIO_P, IDC_RADIO_R,
        IDC_BTN_SAVE, IDC_BTN_CANCEL
    };
    for (size_t i = 0; i < sizeof(editor_ids)/sizeof(editor_ids[0]); i++) {
        HWND c = GetDlgItem(hwnd, editor_ids[i]);
        if (c) ShowWindow(c, SW_HIDE);
    }
    /* Plus all the unidentified STATIC labels we created - hide them by walking
     * children with no ID. Easier: hide by tagging them, but for v1 we leave
     * those labels visible; they're informational. */

    /* Apply font to all */
    HWND child = GetWindow(hwnd, GW_CHILD);
    while (child) {
        SendMessageA(child, WM_SETFONT, (WPARAM)g_font, TRUE);
        child = GetWindow(child, GW_HWNDNEXT);
    }
}

static void kv_show_editor_controls(BOOL show) {
    int show_flag = show ? SW_SHOW : SW_HIDE;
    static const int ids[] = {
        IDC_EDIT_TRIGGER, IDC_BTN_REC_TRIG,
        IDC_EDIT_ACTION,  IDC_BTN_REC_ACT,
        IDC_EDIT_LABEL,
        IDC_RADIO_PRESSED, IDC_RADIO_RELEASED,
        IDC_RADIO_PR, IDC_RADIO_P, IDC_RADIO_R,
        IDC_BTN_SAVE, IDC_BTN_CANCEL,
        IDC_EDITOR_PANEL
    };
    for (size_t i = 0; i < sizeof(ids)/sizeof(ids[0]); i++) {
        HWND c = GetDlgItem(g_main, ids[i]);
        if (c) {
            ShowWindow(c, show_flag);
            /* Belt-and-braces: explicitly enable when showing. Defends
             * against any inherited disabled state from the parent. */
            if (show) EnableWindow(c, TRUE);
        }
    }
}

static void kv_open_editor(int editIdx) {
    kv_show_editor_controls(TRUE);
    kv_editor_show(editIdx);
}

static void kv_close_editor(void) {
    kv_editor_hide();
    kv_show_editor_controls(FALSE);
}

/* Disambiguate VKs that WM_KEYDOWN delivers in ambiguous form:
 *
 *   - VK_PROCESSKEY (0xE5) is delivered when an IME (Korean Hangul,
 *     Japanese IME, Chinese IME, etc.) is intercepting the key. The
 *     real VK can still be recovered from the scancode in lParam,
 *     which the IME does NOT rewrite.
 *
 *   - VK_SHIFT  -> VK_LSHIFT  / VK_RSHIFT   (driven by scancode)
 *   - VK_CONTROL -> VK_LCONTROL / VK_RCONTROL (driven by extended bit)
 *   - VK_MENU   -> VK_LMENU   / VK_RMENU   (driven by extended bit)
 *   - VK_LWIN   -> VK_LWIN    / VK_RWIN    (driven by scancode; some
 *                                            drivers send VK_LWIN for both)
 *
 * lParam bit 24 = extended-key flag. bits 16-23 = OEM scancode.
 */
static BYTE kv_disambiguate_modifier(BYTE vk, LPARAM lParam) {
    UINT scancode = (UINT)((lParam >> 16) & 0xFF);
    BOOL extended = (lParam & (1L << 24)) != 0;
    UINT scForMap = extended ? (scancode | 0xE000) : scancode;

    /* IME interception: recover real VK from scancode. */
    if (vk == VK_PROCESSKEY || vk == 0xFF) {
        UINT mapped = MapVirtualKeyA(scForMap, MAPVK_VSC_TO_VK_EX);
        if (mapped == 0) {
            mapped = MapVirtualKeyA(scancode, MAPVK_VSC_TO_VK);
        }
        if (mapped != 0) vk = (BYTE)mapped;
    }

    /* L/R disambiguation. */
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) {
        UINT mapped = MapVirtualKeyA(scForMap, MAPVK_VSC_TO_VK_EX);
        if (mapped != 0) return (BYTE)mapped;
    }

    /* Win keys: scancode disambiguates. */
    if (vk == VK_LWIN || vk == VK_RWIN) {
        if (scancode == 0x5C) return VK_RWIN;
        if (scancode == 0x5B) return VK_LWIN;
    }
    return vk;
}

/* During capture, route raw keystrokes here. Called from WM_KEYDOWN /
 * WM_KEYUP / WM_SYSKEYDOWN / WM_SYSKEYUP. Returns 0 to consume the
 * message (no further DefWindowProc, no beep). */
static LRESULT kv_handle_capture_key(HWND hwnd, BYTE vk, BOOL keyUp) {
    if (vk == 0) return 0;

    /* Ignore mouse-button VKs and weird high range. */
    if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
        vk == VK_XBUTTON1 || vk == VK_XBUTTON2 || vk == VK_CANCEL) {
        return 0;
    }

    if (keyUp) {
        int done = kv_capture_remove(vk);
        /* Live feedback */
        BYTE keys[KV_MAX_TRIGGER_KEYS];
        int n = kv_capture_result(keys, KV_MAX_TRIGGER_KEYS);
        char keysbuf[128];
        kv_format_keys(keys, n, keysbuf, sizeof(keysbuf));
        char buf[200];
        snprintf(buf, sizeof(buf), "[listening] %s   (release all to finish)",
                 n > 0 ? keysbuf : "(none yet)");
        int field = g_capturingTrigger ? IDC_EDIT_TRIGGER : IDC_EDIT_ACTION;
        SetDlgItemTextA(hwnd, field, buf);
        if (done) {
            /* All keys released and we captured something - finalise. */
            kv_editor_on_capture_done();
        }
    } else {
        kv_capture_add(vk);
        /* Live feedback */
        BYTE keys[KV_MAX_TRIGGER_KEYS];
        int n = kv_capture_result(keys, KV_MAX_TRIGGER_KEYS);
        char keysbuf[128];
        kv_format_keys(keys, n, keysbuf, sizeof(keysbuf));
        char buf[200];
        snprintf(buf, sizeof(buf), "[listening] %s   (release all to finish)",
                 n > 0 ? keysbuf : "(none yet)");
        int field = g_capturingTrigger ? IDC_EDIT_TRIGGER : IDC_EDIT_ACTION;
        SetDlgItemTextA(hwnd, field, buf);
        /* Mirror in status line */
        char statusBuf[256];
        snprintf(statusBuf, sizeof(statusBuf), "Recording %s: %s",
            g_capturingTrigger ? "trigger" : "action", keysbuf);
        kv_set_status(statusBuf);
    }
    return 0;
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    /* While recording, intercept ALL keyboard messages BEFORE the switch
     * dispatch so they never reach DefWindowProc or the focused control
     * (which would beep). */
    if ((g_edState == ED_RECORDING_TRIGGER || g_edState == ED_RECORDING_ACTION) &&
        (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN ||
         msg == WM_KEYUP   || msg == WM_SYSKEYUP)) {
        /* If the user has focus on one of the trigger/action/label EDIT
         * fields, they're typing the key name manually - let the EDIT
         * receive the keystroke instead of capturing it. This is the
         * escape hatch from "I clicked Record but want to type instead". */
        HWND focus = GetFocus();
        HWND triggerEdit = GetDlgItem(g_main, IDC_EDIT_TRIGGER);
        HWND actionEdit  = GetDlgItem(g_main, IDC_EDIT_ACTION);
        HWND labelEdit   = GetDlgItem(g_main, IDC_EDIT_LABEL);
        if (focus == triggerEdit || focus == actionEdit || focus == labelEdit) {
            /* Cancel the recording state since the user moved on. */
            g_edState = g_capturingTrigger ? ED_TRIGGER_DONE : ED_ACTION_DONE;
            kv_set_status(g_hookActive ? "Hook: ACTIVE" : "Hook: inactive");
            /* Fall through to normal dispatch so the EDIT gets the key. */
        } else {
            /* Esc cancels recording, restores focus to the relevant edit. */
            if (msg == WM_KEYDOWN && wParam == VK_ESCAPE) {
                g_edState = g_capturingTrigger ? ED_TRIGGER_DONE : ED_ACTION_DONE;
                kv_set_status(g_hookActive ? "Hook: ACTIVE" : "Hook: inactive");
                SetFocus(g_capturingTrigger ? triggerEdit : actionEdit);
                return 0;
            }
            BOOL keyUp = (msg == WM_KEYUP || msg == WM_SYSKEYUP);
            BYTE vk    = (BYTE)(wParam & 0xFFu);
            vk = kv_disambiguate_modifier(vk, lParam);
            return kv_handle_capture_key(hwnd, vk, keyUp);
        }
    }
    /* Also swallow WM_CHAR/WM_SYSCHAR during capture so DefWindowProc
     * doesn't beep on Alt+key combos - but only when focus is not in
     * an EDIT (same escape-hatch logic). */
    if ((g_edState == ED_RECORDING_TRIGGER || g_edState == ED_RECORDING_ACTION) &&
        (msg == WM_CHAR || msg == WM_SYSCHAR ||
         msg == WM_DEADCHAR || msg == WM_SYSDEADCHAR)) {
        HWND focus = GetFocus();
        HWND triggerEdit = GetDlgItem(g_main, IDC_EDIT_TRIGGER);
        HWND actionEdit  = GetDlgItem(g_main, IDC_EDIT_ACTION);
        HWND labelEdit   = GetDlgItem(g_main, IDC_EDIT_LABEL);
        if (focus != triggerEdit && focus != actionEdit && focus != labelEdit) {
            return 0;
        }
    }

    switch (msg) {
    case WM_CREATE: {
        kv_create_controls(hwnd);
        HMODULE imm = LoadLibraryA("imm32.dll");
        if (imm) {
            typedef HANDLE (WINAPI *pfn_ImmAssociateContext)(HWND, HANDLE);
            pfn_ImmAssociateContext fn =
                (pfn_ImmAssociateContext)(void*)GetProcAddress(imm, "ImmAssociateContext");
            if (fn) fn(hwnd, NULL);
        }
        return 0;
    }

    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        switch (id) {
        case IDC_BTN_CREATE:
            kv_open_editor(-1);
            return 0;
        case IDC_BTN_EDIT: {
            HWND lb = GetDlgItem(hwnd, IDC_LIST_MAPPINGS);
            int sel = (int)SendMessageA(lb, LB_GETCURSEL, 0, 0);
            if (sel >= 0) kv_open_editor(sel);
            return 0;
        }
        case IDC_BTN_DELETE: {
            HWND lb = GetDlgItem(hwnd, IDC_LIST_MAPPINGS);
            int sel = (int)SendMessageA(lb, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < g_shared->count) {
                for (int32_t i = sel; i < g_shared->count - 1; i++) {
                    g_shared->mappings[i] = g_shared->mappings[i + 1];
                }
                g_shared->count--;
                kv_persist_and_publish();
            }
            return 0;
        }
        case IDC_BTN_ABOUT:
        case IDM_TRAY_ABOUT:
            MessageBoxA(hwnd,
                "KVMapper v0.10.9\n"
                "KVM-aware hotkey mapper for Windows.\n\n"
                "(c) Jason Lee. MIT License.",
                "About KVMapper", MB_OK | MB_ICONINFORMATION);
            return 0;
        case IDC_BTN_HIDE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case IDC_BTN_TEST_CLEAR:
            SetDlgItemTextA(hwnd, IDC_EDIT_TEST, "");
            return 0;
        case IDC_BTN_REC_TRIG:
            kv_editor_start_capture(1);
            return 0;
        case IDC_BTN_REC_ACT:
            kv_editor_start_capture(0);
            return 0;
        case IDC_BTN_SAVE:
            kv_editor_save();
            return 0;
        case IDC_BTN_CANCEL:
            kv_close_editor();
            return 0;
        case IDC_LIST_MAPPINGS:
            if (HIWORD(wParam) == LBN_DBLCLK) {
                HWND lb = GetDlgItem(hwnd, IDC_LIST_MAPPINGS);
                int sel = (int)SendMessageA(lb, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < g_shared->count) {
                    g_shared->mappings[sel].enabled = !g_shared->mappings[sel].enabled;
                    kv_persist_and_publish();
                }
            }
            return 0;
        case IDM_TRAY_SHOW:
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            return 0;
        case IDM_TRAY_GITHUB:
            ShellExecuteA(NULL, "open",
                "https://github.com/themanlee7942/KVMapper", NULL, NULL, SW_SHOWNORMAL);
            return 0;
        case IDM_TRAY_EXIT:
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    }

    case WM_TRAY_ICON:
        if (lParam == WM_RBUTTONUP) {
            kv_tray_menu(hwnd);
        } else if (lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        }
        return 0;

    case WM_APP_STOP_REQUEST:
        DestroyWindow(hwnd);
        return 0;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        kv_tray_remove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ---------- /stop handling ---------- */
static BOOL kv_send_stop_signal(void) {
    HANDLE ev = OpenEventA(EVENT_MODIFY_STATE, FALSE, KV_STOP_EVENT_NAME);
    if (!ev) return FALSE;
    SetEvent(ev);
    CloseHandle(ev);
    return TRUE;
}

/* ---------- WinMain ---------- */
int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show) {
    (void)prev;
    g_inst = inst;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    BOOL startTray = FALSE;
    if (cmdline) {
        if (strstr(cmdline, "/stop")) { kv_send_stop_signal(); return 0; }
        if (strstr(cmdline, "/tray")) { startTray = TRUE; }
    }

    g_instMutex = CreateMutexA(NULL, TRUE, KV_MUTEX_NAME);
    if (!g_instMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_instMutex) CloseHandle(g_instMutex);
        /* Don't fail silently - tell the user what happened so they can
         * fix it. Suppress this for /tray (autostart) where silent is OK. */
        if (!startTray) {
            MessageBoxA(NULL,
                "KVMapper is already running.\n\n"
                "Check your system tray (right-click the icon to exit), or run:\n"
                "    kvmapper.exe /stop\n"
                "to terminate the existing instance.",
                "KVMapper",
                MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    }

    kv_resolve_cfg_path();
    g_shared = kv_shm_create();
    if (!g_shared) {
        MessageBoxA(NULL, "Failed to create shared memory.", "KVMapper", MB_OK | MB_ICONERROR);
        return 1;
    }
    kv_config_load(g_cfgPath, g_shared);
    kv_shm_publish();

    g_stopEvent = CreateEventA(NULL, FALSE, FALSE, KV_STOP_EVENT_NAME);

    g_font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    g_titleFont = CreateFontA(-16, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, "Segoe UI");

    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = KV_WND_CLASS;
    wc.hIcon         = kv_load_tray_icon_size(32);
    RegisterClassA(&wc);

    g_main = CreateWindowExA(0, KV_WND_CLASS,
        "KVMapper - KVM Remote Hotkey Mapper",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 780,
        NULL, NULL, inst, NULL);
    if (!g_main) {
        kv_shm_destroy();
        return 1;
    }

    if (kv_load_hook_dll()) {
        kv_hook_start();
    } else {
        kv_set_status("Hook: DLL not found - put kvmapper_hook.dll next to the exe.");
    }
    if (!kv_classifier_start(inst)) {
        kv_set_status("Hook: ACTIVE   (classifier: FAILED to start)");
    }

    kv_list_rebuild();
    kv_tray_add(g_main);

    if (g_stopEvent) {
        DWORD tid = 0;
        g_stopThread = CreateThread(NULL, 0, stop_watcher_proc, NULL, 0, &tid);
    }

    if (startTray) {
        ShowWindow(g_main, SW_HIDE);
    } else {
        ShowWindow(g_main, show);
        UpdateWindow(g_main);
    }

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        BOOL recording = (g_edState == ED_RECORDING_TRIGGER ||
                          g_edState == ED_RECORDING_ACTION);
        if (recording) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        } else if (!IsDialogMessageA(g_main, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    kv_classifier_stop();
    kv_hook_stop();
    if (g_hookDll) FreeLibrary(g_hookDll);
    kv_shm_destroy();
    if (g_stopEvent) CloseHandle(g_stopEvent);
    if (g_stopThread) { TerminateThread(g_stopThread, 0); CloseHandle(g_stopThread); }
    if (g_instMutex) { ReleaseMutex(g_instMutex); CloseHandle(g_instMutex); }
    if (g_font) DeleteObject(g_font);
    if (g_titleFont) DeleteObject(g_titleFont);
    return (int)msg.wParam;
}
