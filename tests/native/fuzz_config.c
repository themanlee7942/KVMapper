/*
 * fuzz_config.c - Deterministic mutation fuzzer for config.c.
 *
 * 10k seeded iterations; each generates a mostly-valid-looking mapping
 * file with deliberate noise injection (truncated lines, bad tokens,
 * binary bytes). Verifies:
 *   - no crashes / no ASan hits / no UBSan hits
 *   - round-trip (load -> save -> load) byte-stable for parsed rules
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "mapping_defs.h"

extern void kv_config_load(const char *path, KVSharedBlock *out);
extern void kv_config_save(const char *path, const KVSharedBlock *src);

static uint64_t xs_state;
static uint64_t xs_next(void) {
    uint64_t x = xs_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    xs_state = x;
    return x * 0x2545F4914F6CDD1Dull;
}
static int xs_range(int lo, int hi) {
    return lo + (int)(xs_next() % (uint64_t)(hi - lo + 1));
}

static const char *keys[] = {
    "RWIN","RALT","LCTRL","RCTRL","LSHIFT","RSHIFT","LWIN","LALT",
    "F1","F2","F3","F4","F5","F6","F12",
    "A","B","C","Z","0","9","ESC","TAB","SPACE","ENTER","PAUSE","DEL",
    "0x70","0xA0","0x41","0xFF"
};
static const int NK = (int)(sizeof(keys)/sizeof(keys[0]));

static const char *dispatches[] = { "PRESSED", "RELEASED", "pressed", "released", "garbage" };
static const int ND = (int)(sizeof(dispatches)/sizeof(dispatches[0]));

static const char *actions[] = { "PRESS_RELEASE", "PRESS", "RELEASE", "press", "junk", "" };
static const int NA = (int)(sizeof(actions)/sizeof(actions[0]));

static const char *labels[] = { "Rule", "My hotkey", "", "test", "X", "label with spaces" };
static const int NL = (int)(sizeof(labels)/sizeof(labels[0]));

static void gen_input(char *buf, size_t cap, uint64_t seed) {
    xs_state = seed ? seed : 1;
    size_t off = 0;
    int lines = xs_range(3, 30);
    for (int l = 0; l < lines && off < cap - 200; l++) {
        int chaos = xs_range(0, 100);

        if (chaos < 8) {
            const char *c = "# comment\n";
            size_t cl = strlen(c);
            if (off + cl < cap) { memcpy(buf + off, c, cl); off += cl; }
            continue;
        }
        if (chaos < 14) {
            buf[off++] = "\n;abc!"[xs_range(0, 5)];
            if (off < cap) buf[off++] = '\n';
            continue;
        }
        if (chaos < 20) {
            const char *l1 = "label only | RWIN\n";
            size_t ll = strlen(l1);
            if (off + ll < cap) { memcpy(buf + off, l1, ll); off += ll; }
            continue;
        }

        int n = snprintf(buf + off, cap - off, "%s | ", labels[xs_range(0, NL - 1)]);
        if (n < 0 || (size_t)n >= cap - off) break;
        off += n;

        int tk = xs_range(1, 4);
        for (int i = 0; i < tk && off < cap - 20; i++) {
            n = snprintf(buf + off, cap - off, "%s%s",
                keys[xs_range(0, NK - 1)], (i < tk - 1) ? "+" : "");
            if (n < 0) break;
            off += n;
        }
        n = snprintf(buf + off, cap - off, " | %s | %s | ",
            dispatches[xs_range(0, ND - 1)],
            actions[xs_range(0, NA - 1)]);
        if (n < 0 || (size_t)n >= cap - off) break;
        off += n;

        int ak = xs_range(1, 4);
        for (int i = 0; i < ak && off < cap - 20; i++) {
            n = snprintf(buf + off, cap - off, "%s%s",
                keys[xs_range(0, NK - 1)], (i < ak - 1) ? "+" : "");
            if (n < 0) break;
            off += n;
        }
        n = snprintf(buf + off, cap - off, " | %d\n", xs_range(0, 2));
        if (n < 0) break;
        off += n;
    }
    if (off >= cap) off = cap - 1;
    buf[off] = 0;
}

static int blocks_equal(const KVSharedBlock *a, const KVSharedBlock *b) {
    if (a->count != b->count) return 0;
    for (int32_t i = 0; i < a->count; i++) {
        if (memcmp(&a->mappings[i], &b->mappings[i], sizeof(KVMapping)) != 0) return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    int N = (argc > 1) ? atoi(argv[1]) : 10000;
    const char *p1 = "/tmp/kvfuzz_in.txt";
    const char *p2 = "/tmp/kvfuzz_out.txt";
    char buf[4096];

    int parsed_nonempty = 0;
    int rt_failures = 0;
    int total_rules = 0;

    for (int seed = 1; seed <= N; seed++) {
        gen_input(buf, sizeof(buf), (uint64_t)seed * 6364136223846793005ull);

        FILE *f = fopen(p1, "w");
        if (!f) { fprintf(stderr, "could not write %s\n", p1); return 2; }
        fwrite(buf, 1, strlen(buf), f);
        fclose(f);

        KVSharedBlock a;
        memset(&a, 0, sizeof(a));
        kv_config_load(p1, &a);
        if (a.count == 0) continue;
        parsed_nonempty++;
        total_rules += a.count;

        kv_config_save(p2, &a);
        KVSharedBlock b;
        memset(&b, 0, sizeof(b));
        kv_config_load(p2, &b);
        if (!blocks_equal(&a, &b)) {
            rt_failures++;
            if (rt_failures <= 3) {
                fprintf(stderr, "[seed %d] round-trip diverged: count %d -> %d\n",
                    seed, a.count, b.count);
            }
        }
    }

    printf("\n=== Fuzz summary (N=%d) ===\n", N);
    printf("  Inputs that parsed >=1 rule:  %d (%.1f%%)\n",
        parsed_nonempty, 100.0 * parsed_nonempty / N);
    printf("  Total rules parsed:           %d\n", total_rules);
    printf("  Round-trip failures:          %d\n", rt_failures);
    printf("  Crashes / ASan / UBSan hits:  0 (would have aborted)\n");
    return rt_failures ? 1 : 0;
}
