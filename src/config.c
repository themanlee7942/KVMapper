/*
 * config.c - VK name table + parse/write kvmapper_mappings.txt.
 *
 * Pure logic, no WinAPI besides the VK_* defines. Unit-testable in
 * isolation.
 *
 * File format (see plan §9):
 *   LABEL | TRIGGER | DISPATCH | ACTION_TYPE | ACTION_KEYS | ENABLED
 *
 * VK names are case-insensitive. Unknown names are skipped silently
 * (we'd rather drop a bad rule than refuse to start).
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "mapping_defs.h"

/* ---------- VK name table ---------- */

typedef struct { const char *name; BYTE vk; } VkName;

static const VkName g_vk_names[] = {
    /* Modifiers - left/right disambiguation matters for KVM */
    {"LWIN",   VK_LWIN},     {"RWIN",  VK_RWIN},
    {"LALT",   VK_LMENU},    {"RALT",  VK_RMENU},
    {"LCTRL",  VK_LCONTROL}, {"RCTRL", VK_RCONTROL},
    {"LSHIFT", VK_LSHIFT},   {"RSHIFT", VK_RSHIFT},
    {"ALT",    VK_MENU},     {"CTRL",  VK_CONTROL},
    {"SHIFT",  VK_SHIFT},    {"WIN",   VK_LWIN},

    /* Function keys */
    {"F1", VK_F1},   {"F2", VK_F2},   {"F3", VK_F3},   {"F4", VK_F4},
    {"F5", VK_F5},   {"F6", VK_F6},   {"F7", VK_F7},   {"F8", VK_F8},
    {"F9", VK_F9},   {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
    {"F13", VK_F13}, {"F14", VK_F14}, {"F15", VK_F15}, {"F16", VK_F16},
    {"F17", VK_F17}, {"F18", VK_F18}, {"F19", VK_F19}, {"F20", VK_F20},
    {"F21", VK_F21}, {"F22", VK_F22}, {"F23", VK_F23}, {"F24", VK_F24},

    /* IME / East Asian language keys - using the official Windows VK_*
     * names. VK_HANGUL == VK_KANA == 0x15 (Windows aliases them in
     * winuser.h - same VK, same effect). Similarly VK_HANJA == VK_KANJI
     * == 0x19. */
    {"HANGUL",     0x15}, {"KANA",       0x15},
    {"HANJA",      0x19}, {"KANJI",      0x19},
    {"IME_ON",     0x16}, {"IME_OFF",    0x1A},  /* Win10+ */
    {"CONVERT",    0x1C}, {"NONCONVERT", 0x1D},

    /* Multimedia / system keys (Windows naming) */
    {"SLEEP",            0x5F},
    {"VOLUME_MUTE",      0xAD},
    {"VOLUME_DOWN",      0xAE},
    {"VOLUME_UP",        0xAF},
    {"MEDIA_NEXT_TRACK", 0xB0}, {"MEDIANEXT", 0xB0},
    {"MEDIA_PREV_TRACK", 0xB1}, {"MEDIAPREV", 0xB1},
    {"MEDIA_STOP",       0xB2}, {"MEDIASTOP", 0xB2},
    {"MEDIA_PLAY_PAUSE", 0xB3}, {"MEDIAPLAY", 0xB3},

    /* Browser keys */
    {"BROWSER_BACK",      0xA6},
    {"BROWSER_FORWARD",   0xA7},
    {"BROWSER_REFRESH",   0xA8},
    {"BROWSER_STOP",      0xA9},
    {"BROWSER_SEARCH",    0xAA},
    {"BROWSER_FAVORITES", 0xAB},
    {"BROWSER_HOME",      0xAC},

    /* App-launch keys (the e-mail / media / generic app keys on some
     * multimedia keyboards). */
    {"LAUNCH_MAIL",         0xB4},
    {"LAUNCH_MEDIA_SELECT", 0xB5},
    {"LAUNCH_APP1",         0xB6},
    {"LAUNCH_APP2",         0xB7},

    /* Other less common but useful VKs */
    {"CLEAR",   0x0C},
    {"SELECT",  0x29},
    {"PRINT",   0x2A},
    {"EXECUTE", 0x2B},
    {"HELP",    0x2F},

    /* Specials */
    {"ESC",    VK_ESCAPE}, {"TAB",   VK_TAB},   {"SPACE", VK_SPACE},
    {"ENTER",  VK_RETURN}, {"RETURN", VK_RETURN},
    {"BACK",   VK_BACK},   {"BACKSPACE", VK_BACK},
    {"PAUSE",  VK_PAUSE},  {"SCROLL", VK_SCROLL},
    {"CAPS",   VK_CAPITAL}, {"CAPSLOCK", VK_CAPITAL},
    {"NUMLOCK", VK_NUMLOCK},
    {"PRTSC",  VK_SNAPSHOT}, {"PRINTSCREEN", VK_SNAPSHOT},
    {"APPS",   VK_APPS},
    {"INS",    VK_INSERT}, {"INSERT", VK_INSERT},
    {"DEL",    VK_DELETE}, {"DELETE", VK_DELETE},
    {"HOME",   VK_HOME},   {"END",    VK_END},
    {"PGUP",   VK_PRIOR},  {"PAGEUP", VK_PRIOR},
    {"PGDN",   VK_NEXT},   {"PAGEDOWN", VK_NEXT},
    {"UP",     VK_UP},     {"DOWN",   VK_DOWN},
    {"LEFT",   VK_LEFT},   {"RIGHT",  VK_RIGHT},

    /* Numpad */
    {"NUM0", VK_NUMPAD0}, {"NUM1", VK_NUMPAD1}, {"NUM2", VK_NUMPAD2},
    {"NUM3", VK_NUMPAD3}, {"NUM4", VK_NUMPAD4}, {"NUM5", VK_NUMPAD5},
    {"NUM6", VK_NUMPAD6}, {"NUM7", VK_NUMPAD7}, {"NUM8", VK_NUMPAD8},
    {"NUM9", VK_NUMPAD9},
    {"NUMADD", VK_ADD}, {"NUMSUB", VK_SUBTRACT},
    {"NUMMUL", VK_MULTIPLY}, {"NUMDIV", VK_DIVIDE},
    {"NUMDOT", VK_DECIMAL},

    /* OEM punctuation - rough, US-layout names */
    {"SEMICOLON", VK_OEM_1},     {"SLASH", VK_OEM_2},
    {"BACKTICK",  VK_OEM_3},     {"LBRACKET", VK_OEM_4},
    {"BACKSLASH", VK_OEM_5},     {"RBRACKET", VK_OEM_6},
    {"QUOTE",     VK_OEM_7},     {"COMMA",   VK_OEM_COMMA},
    {"PERIOD",    VK_OEM_PERIOD}, {"MINUS",  VK_OEM_MINUS},
    {"PLUS",      VK_OEM_PLUS},

    /* Sentinel */
    {NULL, 0}
};

static int kv_strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = toupper((unsigned char)*a);
        int cb = toupper((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (int)((unsigned char)toupper((unsigned char)*a)
               - (unsigned char)toupper((unsigned char)*b));
}

/* Convert a single VK token (case-insensitive name, A-Z, 0-9, or 0xNN)
 * to a VK code. Returns 0 if unrecognised. */
BYTE kv_vk_from_name(const char *name) {
    if (!name || !*name) return 0;

    /* Hex literal: 0xNN */
    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        unsigned int v = 0;
        if (sscanf(name + 2, "%x", &v) == 1 && v <= 0xFF) {
            return (BYTE)v;
        }
        return 0;
    }

    /* Single letter A-Z -> VK matches ASCII */
    if (!name[1] && name[0] >= 'a' && name[0] <= 'z') return (BYTE)(name[0] - 'a' + 'A');
    if (!name[1] && name[0] >= 'A' && name[0] <= 'Z') return (BYTE)name[0];

    /* Single digit 0-9 -> VK matches ASCII */
    if (!name[1] && name[0] >= '0' && name[0] <= '9') return (BYTE)name[0];

    for (const VkName *p = g_vk_names; p->name; p++) {
        if (kv_strcasecmp(name, p->name) == 0) return p->vk;
    }
    return 0;
}

/* Reverse lookup: VK -> name. Prefers the canonical name (first match
 * in the table). For unknown VKs returns NULL; caller should fall back
 * to "0xNN" formatting. */
const char *kv_name_from_vk(BYTE vk) {
    /* A-Z, 0-9 short-circuit */
    static char one[2];
    if (vk >= 'A' && vk <= 'Z') { one[0] = (char)vk; one[1] = 0; return one; }
    if (vk >= '0' && vk <= '9') { one[0] = (char)vk; one[1] = 0; return one; }

    for (const VkName *p = g_vk_names; p->name; p++) {
        if (p->vk == vk) return p->name;
    }
    return NULL;
}

/* ---------- Trimming + token helpers ---------- */

static char *kv_trim(char *s) {
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = 0;
    return s;
}

int kv_parse_vk_list(const char *src, BYTE *out, int max) {
    if (!src || !out) return 0;
    char buf[256];
    strncpy(buf, src, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    int n = 0;
    char *save = NULL;
    /* Use strtok over '+' and whitespace */
    for (char *tok = strtok_s(buf, "+ \t", &save);
         tok && n < max;
         tok = strtok_s(NULL, "+ \t", &save))
    {
        BYTE vk = kv_vk_from_name(kv_trim(tok));
        if (vk) out[n++] = vk;
    }
    return n;
}

static const char *kv_dispatch_str(int32_t d) {
    return (d == KV_DISPATCH_RELEASED) ? "RELEASED" : "PRESSED";
}

static const char *kv_action_type_str(int32_t t) {
    switch (t) {
        case KV_ACTION_PRESS:   return "PRESS";
        case KV_ACTION_RELEASE: return "RELEASE";
        default:                return "PRESS_RELEASE";
    }
}

static int32_t kv_action_type_from_str(const char *s) {
    if (kv_strcasecmp(s, "PRESS") == 0)    return KV_ACTION_PRESS;
    if (kv_strcasecmp(s, "RELEASE") == 0)  return KV_ACTION_RELEASE;
    return KV_ACTION_PRESS_RELEASE;
}

static int32_t kv_dispatch_from_str(const char *s) {
    if (kv_strcasecmp(s, "RELEASED") == 0) return KV_DISPATCH_RELEASED;
    return KV_DISPATCH_PRESSED;
}

static const char *kv_source_filter_str(int32_t s) {
    switch (s) {
        case KV_SOURCE_LOCAL:  return "LOCAL";
        case KV_SOURCE_REMOTE: return "REMOTE";
        default:               return "ANY";
    }
}

static int32_t kv_source_filter_from_str(const char *s) {
    if (kv_strcasecmp(s, "LOCAL") == 0)  return KV_SOURCE_LOCAL;
    if (kv_strcasecmp(s, "REMOTE") == 0) return KV_SOURCE_REMOTE;
    return KV_SOURCE_ANY;
}

static void kv_format_vk_list(const BYTE *keys, int32_t n, char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = 0;
    for (int32_t i = 0; i < n; i++) {
        const char *nm = kv_name_from_vk(keys[i]);
        char tmp[16];
        if (!nm) {
            snprintf(tmp, sizeof(tmp), "0x%02X", keys[i]);
            nm = tmp;
        }
        if (i > 0) strncat(out, "+", cap - strlen(out) - 1);
        strncat(out, nm, cap - strlen(out) - 1);
    }
}

/* ---------- Public load/save ---------- */

void kv_config_load(const char *path, KVSharedBlock *out) {
    if (!out) return;
    out->count = 0;
    if (!path) return;

    FILE *f = NULL;
    if (fopen_s(&f, path, "r") != 0 || !f) return;

    char line[1024];
    while (fgets(line, sizeof(line), f) && out->count < KV_MAX_MAPPINGS) {
        char *p = kv_trim(line);
        if (!*p || *p == '#') continue;

        /* Split by '|'. Up to 7 fields (label, trig, disp, atype,
         * akeys, enabled, sourceFilter). The 7th is optional - v0.9
         * files have 6 fields and parse cleanly. */
        char *fields[7] = {0};
        int fn = 0;
        char *save = NULL;
        for (char *tok = strtok_s(p, "|", &save);
             tok && fn < 7;
             tok = strtok_s(NULL, "|", &save))
        {
            fields[fn++] = kv_trim(tok);
        }
        if (fn < 5) continue;   /* malformed - skip */

        KVMapping *m = &out->mappings[out->count];
        memset(m, 0, sizeof(*m));

        strncpy(m->label, fields[0], sizeof(m->label) - 1);
        m->triggerCount = kv_parse_vk_list(fields[1], m->triggerKeys, KV_MAX_TRIGGER_KEYS);
        m->dispatch     = kv_dispatch_from_str(fields[2]);
        m->actionType   = kv_action_type_from_str(fields[3]);
        m->actionCount  = kv_parse_vk_list(fields[4], m->actionKeys, KV_MAX_ACTION_KEYS);
        m->enabled      = (fn >= 6 && fields[5][0] == '0') ? 0 : 1;
        /* Optional 7th field: source filter. Missing or unrecognised -> ANY. */
        m->sourceFilter = (fn >= 7) ? kv_source_filter_from_str(fields[6])
                                    : KV_SOURCE_ANY;

        if (m->triggerCount > 0 && m->actionCount > 0) {
            out->count++;
        }
    }
    fclose(f);
}

/* Returns 1 on success, 0 on failure (read-only folder, denied ACL, AV, etc.) */
int kv_config_save(const char *path, const KVSharedBlock *src) {
    if (!path || !src) return 0;
    FILE *f = NULL;
    if (fopen_s(&f, path, "w") != 0 || !f) return 0;

    fprintf(f,
        "# KVMapper Mappings - v1.1 (v0.10 adds optional SOURCE field)\n"
        "# DO NOT EDIT MANUALLY WHILE KVMAPPER IS RUNNING\n"
        "#\n"
        "# FORMAT:  LABEL | TRIGGER | DISPATCH | ACTION_TYPE | ACTION_KEYS | ENABLED | SOURCE\n"
        "# Keys:    VK names (LWIN, RALT, F1, A-Z, 0-9) or 0xNN hex.\n"
        "# SOURCE:  ANY | LOCAL | REMOTE   (default ANY if field omitted)\n"
        "# TRIGGER: key+key+key (up to 4)\n"
        "# DISPATCH: PRESSED | RELEASED\n"
        "# ACTION_TYPE: PRESS_RELEASE | PRESS | RELEASE\n"
        "# ACTION_KEYS: key+key+key (up to 4)\n"
        "# ENABLED: 1 | 0\n"
        "\n");

    for (int32_t i = 0; i < src->count; i++) {
        const KVMapping *m = &src->mappings[i];
        char trg[128], act[128];
        kv_format_vk_list(m->triggerKeys, m->triggerCount, trg, sizeof(trg));
        kv_format_vk_list(m->actionKeys,  m->actionCount,  act, sizeof(act));

        fprintf(f, "%s | %s | %s | %s | %s | %d | %s\n",
            m->label[0] ? m->label : "(unnamed)",
            trg,
            kv_dispatch_str(m->dispatch),
            kv_action_type_str(m->actionType),
            act,
            m->enabled ? 1 : 0,
            kv_source_filter_str(m->sourceFilter));
    }
    fclose(f);
    return 1;
}

/* Format a mapping for the UI list. */
void kv_format_mapping_label(const KVMapping *m, char *out, size_t cap) {
    if (!out || cap == 0) return;
    char trg[128], act[128];
    kv_format_vk_list(m->triggerKeys, m->triggerCount, trg, sizeof(trg));
    kv_format_vk_list(m->actionKeys,  m->actionCount,  act, sizeof(act));

    const char *arrow = (m->dispatch == KV_DISPATCH_RELEASED) ? "(on release)" : "(on press)";
    if (m->label[0]) {
        snprintf(out, cap, "%s   %s -> %s  %s",
            m->label, trg, act, arrow);
    } else {
        snprintf(out, cap, "%s -> %s  %s", trg, act, arrow);
    }
}
