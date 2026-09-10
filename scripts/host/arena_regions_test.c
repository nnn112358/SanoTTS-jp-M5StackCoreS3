/* 複数ブロック arena のホストテスト（コアの saan_arena_add / saan_alloc）。
 *
 *   cc -std=c99 -O2 -Icomponents/saanotts_core -Imain scripts/host/arena_regions_test.c \
 *      components/saanotts_core/{saanotts,saanotts_stream,fft,saanotts_int8}.c -lm -o /tmp/arena_test && /tmp/arena_test
 *
 * 1) 割り当てがブロック境界を越えるときの挙動（尻尾を捨てて次へ / 粘着失敗 / mark-rollback）
 * 2) 本物のコアで同じ ids を「1 本」と「複数ブロック」の arena で合成し、PCM の FNV-1a が bit 一致すること */
#include "saanotts.h"
#include "saanotts_stream.h"
#include "saanotts_internal.h"   /* saan_alloc */
#include "demo_ids.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA (176 * 1024)

static int fails;
#define CHECK(c, ...) do { if (!(c)) { ++fails; printf("NG: " __VA_ARGS__); printf("\n"); } } while (0)

static void *slurp(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "開けない: %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "読めない\n"); exit(1); }
    fclose(f); *size = (size_t)n; return b;
}

static uint64_t fnv_pcm(const float *pcm, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        long v = lrintf(pcm[i] * 32767.0f);
        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
        uint16_t u = (uint16_t)(int16_t)v;
        h = (h ^ (uint8_t)(u & 0xff)) * 1099511628211ull;
        h = (h ^ (uint8_t)(u >> 8)) * 1099511628211ull;
    }
    return h;
}

static void test_alloc(void) {
    static __attribute__((aligned(16))) uint8_t b0[1024], b1[512], b2[2048];
    saan_arena a;
    saan_arena_init(&a, b0, sizeof b0);
    CHECK(saan_arena_add(&a, b1, sizeof b1) == 0, "add 1");
    CHECK(saan_arena_add(&a, b2, sizeof b2) == 0, "add 2");
    CHECK(a.size == 1024 + 512 + 2048, "size = 合計");
    void *p = saan_alloc(&a, 1000);                 /* ブロック 0 に 1008 B */
    CHECK(p == b0, "1 つ目はブロック 0 の先頭");
    CHECK(a.used == 1008, "used 1008 (%u)", (unsigned)a.used);
    void *q = saan_alloc(&a, 100);                  /* ブロック 0 の残り 16 B に入らない → ブロック 1 */
    CHECK(q == b1, "入らなければ次のブロック");
    CHECK(a.used == 1008 + 112, "used は生きている確保の合計 (%u)", (unsigned)a.used);
    void *tiny = saan_alloc(&a, 8);                 /* 16 B はブロック 0 の尻尾に入る（first-fit） */
    CHECK(tiny == b0 + 1008, "小さいものはブロック 0 の尻尾に戻る");
    size_t mark = a.used;
    void *r = saan_alloc(&a, 600);                  /* ブロック 1 の残り 400 に入らない → ブロック 2 */
    CHECK(r == b2, "ブロック 2 へ");
    a.used = mark;                                  /* rollback（r を返す） */
    void *r2 = saan_alloc(&a, 300);                 /* ブロック 1 の続き */
    CHECK(r2 == b1 + 112, "rollback 後はブロック 1 の続き (%p vs %p)", r2, (void *)(b1 + 112));
    a.used -= 304;                                  /* `used -= n` でも戻る */
    void *r3 = saan_alloc(&a, 300);
    CHECK(r3 == b1 + 112, "used -= n でも同じ場所");
    void *big = saan_alloc(&a, 3000);               /* どこにも入らない */
    CHECK(big == NULL && a.failed, "入らなければ NULL + 粘着失敗");
    CHECK(saan_alloc(&a, 1) == NULL, "粘着: 以後は小さくても NULL");
    saan_arena_reset(&a);
    CHECK(a.used == 0 && !a.failed, "reset");
    CHECK(saan_alloc(&a, 2048) == b2, "2048 はブロック 2 にだけ入る");
    a.used = 5;                                      /* 境界に合わない戻し方 */
    CHECK(saan_alloc(&a, 16) == NULL && a.failed, "境界に合わない rollback は粘着失敗");
    printf("alloc: %s\n", fails ? "NG" : "OK");
}

static uint64_t synth(saan_arena *a, const saan_weights *w, size_t *n_out, size_t *used_out, size_t *peak_out) {
    saan_stream st;
    saan_status s = saan_stream_init(&st, w, a, kSaanDemoIds, SAAN_DEMO_N_IDS, 1.0f);
    if (s != SAAN_OK) { printf("stream_init: %s\n", saan_strerror(s)); exit(2); }
    *used_out = a->used;
    static float chunk[SAAN_CHUNK * SAAN_HOP];
    static float pcm[350 * 80 * SAAN_HOP / 8];   /* 十分大きい */
    size_t n = 0; int32_t k = 0;
    while (saan_stream_pull(&st, chunk, &k) == SAAN_OK && k > 0) {
        memcpy(pcm + n, chunk, (size_t)k * SAAN_HOP * sizeof(float));
        n += (size_t)k * SAAN_HOP;
    }
    *n_out = n; *peak_out = a->peak;
    return fnv_pcm(pcm, n);
}

int main(int argc, char **argv) {
    test_alloc();
    const char *blob = argc > 1 ? argv[1] : "model/student_i8.bin";
    size_t sz = 0; void *b = slurp(blob, &sz);
    saan_weights w;
    if (saan_weights_open(&w, b, sz) != SAAN_OK) { printf("weights_open 失敗\n"); return 2; }

    static __attribute__((aligned(16))) uint8_t one[ARENA];
    saan_arena a1; saan_arena_init(&a1, one, sizeof one);
    size_t n1, u1, p1; uint64_t h1 = synth(&a1, &w, &n1, &u1, &p1);
    CHECK(u1 == saan_stream_arena_used(SAAN_DEMO_N_IDS), "1 本: used == saan_stream_arena_used (%u vs %u)",
          (unsigned)u1, (unsigned)saan_stream_arena_used(SAAN_DEMO_N_IDS));
    printf("1 本   : %u sample / FNV 0x%016llx / used %u / peak %u\n", (unsigned)n1, (unsigned long long)h1, (unsigned)u1, (unsigned)p1);

    /* ESP32 の実情に近い分け方: 100 KB + 76 KB、および 64 KB × 3 */
    static __attribute__((aligned(16))) uint8_t r0[100 * 1024], r1[76 * 1024];
    saan_arena a2; saan_arena_init(&a2, r0, sizeof r0); saan_arena_add(&a2, r1, sizeof r1);
    size_t n2, u2, p2; uint64_t h2 = synth(&a2, &w, &n2, &u2, &p2);
    printf("2 本   : %u sample / FNV 0x%016llx / used %u / peak %u (%d ブロック)\n", (unsigned)n2, (unsigned long long)h2, (unsigned)u2, (unsigned)p2, a2.n_regions);
    CHECK(n2 == n1 && h2 == h1, "100+76 KB の 2 本で PCM が bit 一致");
    CHECK(u2 == u1, "2 本でも used は同じ（尻尾は数えない）(%u vs %u)", (unsigned)u2, (unsigned)u1);

    static __attribute__((aligned(16))) uint8_t s0[64 * 1024], s1[64 * 1024], s2[64 * 1024];
    saan_arena a3; saan_arena_init(&a3, s0, sizeof s0); saan_arena_add(&a3, s1, sizeof s1); saan_arena_add(&a3, s2, sizeof s2);
    size_t n3, u3, p3; uint64_t h3 = synth(&a3, &w, &n3, &u3, &p3);
    printf("3 本   : %u sample / FNV 0x%016llx / used %u / peak %u (%d ブロック)\n", (unsigned)n3, (unsigned long long)h3, (unsigned)u3, (unsigned)p3, a3.n_regions);
    CHECK(n3 == n1 && h3 == h1, "64 KB x 3 で PCM が bit 一致");

    /* 足りない分け方は init が SAAN_ERR_ARENA で止まる（黙って続かない） */
    static __attribute__((aligned(16))) uint8_t t0[40 * 1024], t1[40 * 1024];
    saan_arena a4; saan_arena_init(&a4, t0, sizeof t0); saan_arena_add(&a4, t1, sizeof t1);
    saan_stream st4;
    saan_status s4 = saan_stream_init(&st4, &w, &a4, kSaanDemoIds, SAAN_DEMO_N_IDS, 1.0f);
    CHECK(s4 == SAAN_ERR_ARENA && a4.failed, "40+40 KB では SAAN_ERR_ARENA (%s)", saan_strerror(s4));

    printf("%s\n", fails ? "FAIL" : "ALL OK");
    return fails ? 1 : 0;
}
