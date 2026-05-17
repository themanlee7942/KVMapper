#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "mapping_defs.h"

extern void kv_config_load(const char *path, KVSharedBlock *out);
extern void kv_config_save(const char *path, const KVSharedBlock *src);
extern BYTE kv_vk_from_name(const char *name);
extern const char *kv_name_from_vk(BYTE vk);

#define TEST(name) do { printf("[TEST] %-58s ", name); fflush(stdout); } while(0)
#define OK() printf("PASS\n")
#define FAIL(m) do { printf("FAIL: %s\n", m); failed++; } while(0)

int failed = 0;

static void test_vk_names(void) {
    TEST("VK name lookup: RWIN");
    if (kv_vk_from_name("RWIN") == VK_RWIN) OK(); else FAIL("RWIN != VK_RWIN");

    TEST("VK name lookup: case-insensitive (rwin)");
    if (kv_vk_from_name("rwin") == VK_RWIN) OK(); else FAIL("rwin != VK_RWIN");

    TEST("VK name lookup: A-Z single letter");
    if (kv_vk_from_name("A") == 'A' && kv_vk_from_name("Z") == 'Z') OK();
    else FAIL("A or Z mismatch");

    TEST("VK name lookup: 0-9 digit");
    if (kv_vk_from_name("0") == '0' && kv_vk_from_name("9") == '9') OK();
    else FAIL("0 or 9 mismatch");

    TEST("VK name lookup: 0xNN hex");
    if (kv_vk_from_name("0x70") == 0x70 && kv_vk_from_name("0xFF") == 0xFF) OK();
    else FAIL("hex literal failed");

    TEST("VK name lookup: F12");
    if (kv_vk_from_name("F12") == VK_F12) OK(); else FAIL("F12 != VK_F12");

    TEST("VK name lookup: unknown returns 0");
    if (kv_vk_from_name("nonexistent") == 0) OK(); else FAIL("unknown should be 0");

    TEST("VK reverse lookup: VK_RWIN -> RWIN");
    const char *n = kv_name_from_vk(VK_RWIN);
    if (n && strcmp(n, "RWIN") == 0) OK(); else FAIL("VK_RWIN -> not RWIN");

    TEST("VK reverse lookup: 'A' (0x41) -> A");
    n = kv_name_from_vk('A');
    if (n && strcmp(n, "A") == 0) OK(); else FAIL("A reverse failed");
}

static void test_parse_save(void) {
    const char *p = "/tmp/cfgtest/sample.txt";
    KVSharedBlock blk;
    memset(&blk, 0, sizeof(blk));

    FILE *f = fopen(p, "w");
    fprintf(f,
        "# A comment\n"
        "\n"
        "Open macro | RWIN+RALT | PRESSED | PRESS_RELEASE | F1 | 1\n"
        "Copy remap | RCTRL+RALT | RELEASED | PRESS_RELEASE | LCTRL+C | 1\n"
        "Disabled rule | RSHIFT | PRESSED | PRESS | LSHIFT | 0\n"
        "Hex test | 0x70 | PRESSED | PRESS_RELEASE | 0xA0+0x41 | 1\n"
        "# Another comment\n"
        "Bad line missing fields\n"
        "Only label | | | | |\n"
    );
    fclose(f);

    TEST("parse: load 4 valid rules");
    kv_config_load(p, &blk);
    if (blk.count == 4) OK();
    else { printf("got %d ", (int)blk.count); FAIL("expected 4"); }

    TEST("parse: first rule label");
    if (blk.count >= 1 && strcmp(blk.mappings[0].label, "Open macro") == 0) OK();
    else FAIL("label mismatch");

    TEST("parse: first rule trigger = RWIN+RALT");
    if (blk.count >= 1 &&
        blk.mappings[0].triggerCount == 2 &&
        blk.mappings[0].triggerKeys[0] == VK_RWIN &&
        blk.mappings[0].triggerKeys[1] == VK_RMENU) OK();
    else FAIL("trigger keys wrong");

    TEST("parse: first rule action = F1");
    if (blk.count >= 1 &&
        blk.mappings[0].actionCount == 1 &&
        blk.mappings[0].actionKeys[0] == VK_F1) OK();
    else FAIL("action wrong");

    TEST("parse: first rule dispatch = PRESSED");
    if (blk.count >= 1 && blk.mappings[0].dispatch == KV_DISPATCH_PRESSED) OK();
    else FAIL("dispatch wrong");

    TEST("parse: second rule dispatch = RELEASED");
    if (blk.count >= 2 && blk.mappings[1].dispatch == KV_DISPATCH_RELEASED) OK();
    else FAIL("RELEASED parse failed");

    TEST("parse: third rule enabled = 0");
    if (blk.count >= 3 && blk.mappings[2].enabled == 0) OK();
    else FAIL("enabled=0 not honored");

    TEST("parse: third rule actionType = PRESS only");
    if (blk.count >= 3 && blk.mappings[2].actionType == KV_ACTION_PRESS) OK();
    else FAIL("PRESS-only not parsed");

    TEST("parse: hex literal trigger 0x70 = VK_F1");
    if (blk.count >= 4 && blk.mappings[3].triggerKeys[0] == VK_F1) OK();
    else FAIL("hex literal failed");

    TEST("parse: hex+name action 0xA0+0x41 = LSHIFT+A");
    if (blk.count >= 4 &&
        blk.mappings[3].actionCount == 2 &&
        blk.mappings[3].actionKeys[0] == VK_LSHIFT &&
        blk.mappings[3].actionKeys[1] == 'A') OK();
    else FAIL("mixed hex action failed");

    const char *p2 = "/tmp/cfgtest/sample-rt.txt";
    kv_config_save(p2, &blk);
    KVSharedBlock blk2;
    memset(&blk2, 0, sizeof(blk2));
    kv_config_load(p2, &blk2);

    TEST("round-trip: count preserved");
    if (blk2.count == blk.count) OK();
    else FAIL("count diverged");

    TEST("round-trip: rule 0 byte-for-byte match");
    if (blk2.count >= 1 && memcmp(&blk.mappings[0], &blk2.mappings[0], sizeof(KVMapping)) == 0) OK();
    else FAIL("rule 0 mismatch");

    TEST("round-trip: rule 2 (disabled) preserves enabled=0");
    if (blk2.count >= 3 && blk2.mappings[2].enabled == 0) OK();
    else FAIL("disabled flag lost");
}

static void test_source_filter(void) {
    const char *p = "/tmp/cfgtest/sample-sf.txt";
    KVSharedBlock blk;
    memset(&blk, 0, sizeof(blk));

    FILE *f = fopen(p, "w");
    fprintf(f,
        "# v0.10 mixed: 6-field (back-compat) + 7-field (new)\n"
        "v0.9 style    | RWIN+RALT | PRESSED  | PRESS_RELEASE | F1 | 1\n"
        "Remote only   | RWIN+RALT | PRESSED  | PRESS_RELEASE | F1 | 1 | REMOTE\n"
        "Local only    | RWIN+RALT | PRESSED  | PRESS_RELEASE | F1 | 1 | LOCAL\n"
        "Explicit any  | RWIN+RALT | PRESSED  | PRESS_RELEASE | F1 | 1 | ANY\n"
        "Unknown src   | RWIN+RALT | PRESSED  | PRESS_RELEASE | F1 | 1 | banana\n"
    );
    fclose(f);
    kv_config_load(p, &blk);

    TEST("source: 6-field old file -> sourceFilter=ANY (back-compat)");
    if (blk.count >= 1 && blk.mappings[0].sourceFilter == KV_SOURCE_ANY) OK();
    else FAIL("v0.9 file lost ANY default");

    TEST("source: 7-field REMOTE parsed");
    if (blk.count >= 2 && blk.mappings[1].sourceFilter == KV_SOURCE_REMOTE) OK();
    else FAIL("REMOTE not parsed");

    TEST("source: 7-field LOCAL parsed");
    if (blk.count >= 3 && blk.mappings[2].sourceFilter == KV_SOURCE_LOCAL) OK();
    else FAIL("LOCAL not parsed");

    TEST("source: explicit ANY parsed");
    if (blk.count >= 4 && blk.mappings[3].sourceFilter == KV_SOURCE_ANY) OK();
    else FAIL("ANY not parsed");

    TEST("source: unknown token -> ANY (safe default)");
    if (blk.count >= 5 && blk.mappings[4].sourceFilter == KV_SOURCE_ANY) OK();
    else FAIL("unknown source not defaulted to ANY");

    const char *p2 = "/tmp/cfgtest/sample-sf-rt.txt";
    kv_config_save(p2, &blk);
    KVSharedBlock blk2;
    memset(&blk2, 0, sizeof(blk2));
    kv_config_load(p2, &blk2);

    TEST("source: round-trip preserves all sourceFilter values");
    int ok = (blk2.count == blk.count);
    for (int i = 0; ok && i < blk.count; i++) {
        if (blk.mappings[i].sourceFilter != blk2.mappings[i].sourceFilter) ok = 0;
    }
    if (ok) OK(); else FAIL("round-trip diverged");
}

static void test_edge_cases(void) {
    TEST("load: nonexistent file -> count 0");
    KVSharedBlock blk;
    memset(&blk, 0, sizeof(blk));
    kv_config_load("/tmp/nonexistent-12345.txt", &blk);
    if (blk.count == 0) OK(); else FAIL("should be 0");

    TEST("load: NULL path safe");
    memset(&blk, 0, sizeof(blk));
    kv_config_load(NULL, &blk);
    if (blk.count == 0) OK(); else FAIL("NULL path crashed?");

    TEST("load: NULL out safe (no crash)");
    kv_config_load("/tmp/whatever", NULL);
    OK();
}

int main(void) {
    /* Ensure tmpdir exists for fopen() calls. */
    system("mkdir -p /tmp/cfgtest");
    test_vk_names();
    test_parse_save();
    test_source_filter();
    test_edge_cases();
    printf("\n");
    if (failed) { printf("%d FAILED\n", failed); return 1; }
    printf("ALL TESTS PASSED\n");
    return 0;
}
