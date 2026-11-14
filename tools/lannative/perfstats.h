// perfstats.h - always-on frame-time recorder (config "perf_stats", secore.inc se_tick). Pure C, no Windows, offline-testable (test\perfstats_test.c).
// One PfHist = a 1 ms histogram (0..199 ms, last bin = 200 ms and more) + count / sum / max. Cost per frame: one bin increment for "all" and one for the kind in hand.
// Fine 1 ms bins (not just the 7 report bands) so the 1% / 0.1% lows come out to +-0.5 ms without keeping every frame.
// Intervals of PF_STALL_MS or more (map loads, a paused window) are not frames: they are counted as stalls and kept out of every statistic.
#ifndef PERFSTATS_H
#define PERFSTATS_H
#include <stdio.h>
#include <string.h>
#include <math.h>
#define PF_BINS 201
#define PF_KINDS 8
#define PF_STALL_MS 1000.0
typedef struct { unsigned n; unsigned bin[PF_BINS]; double sum, max, over_sum; } PfHist;                        // ms
typedef struct { PfHist all; PfHist kind[PF_KINDS]; char kname[PF_KINDS][24]; int nk; unsigned stalls; double stall_ms; } PfRec;

static void pf_hist_add(PfHist *h, double ms) {
    int b = ms < 0 ? 0 : ms >= PF_BINS - 1 ? PF_BINS - 1 : (int)ms;
    h->bin[b]++; h->n++; h->sum += ms; if (ms > h->max) h->max = ms; if (b == PF_BINS - 1) h->over_sum += ms;
}
static void pf_hist_merge(PfHist *d, const PfHist *s) {
    for (int i = 0; i < PF_BINS; i++) d->bin[i] += s->bin[i];
    d->n += s->n; d->sum += s->sum; d->over_sum += s->over_sum; if (s->max > d->max) d->max = s->max;
}
// report bands: <8, 8-16, 16-25, 25-33, 33-50, 50-100, >=100 ms (edges on whole ms)
static const int PF_EDGE[6] = { 8, 16, 25, 33, 50, 100 };
static void pf_bands(const PfHist *h, unsigned out[7]) {
    memset(out, 0, 7 * sizeof out[0]); int band = 0;
    for (int i = 0; i < PF_BINS; i++) { while (band < 6 && i >= PF_EDGE[band]) band++; out[band] += h->bin[i]; }
}
// mean frame time (ms) of the slowest `frac` of the frames (frac 0.01 = the "1% low"; at least one frame); 0 when empty. Bin centres, the overflow bin uses its real sum.
static double pf_low_ms(const PfHist *h, double frac) {
    if (!h->n) return 0; double want = ceil(frac * h->n); if (want < 1) want = 1; double left = want, acc = 0;
    for (int i = PF_BINS - 1; i >= 0 && left > 0; i--) {
        if (!h->bin[i]) continue; double take = h->bin[i] < left ? h->bin[i] : left;
        double each = i == PF_BINS - 1 ? h->over_sum / h->bin[i] : i + 0.5; acc += take * each; left -= take;
    }
    return acc / want;
}
static void pf_frame(PfRec *r, double ms, const char *kind) {
    if (ms >= PF_STALL_MS) { r->stalls++; r->stall_ms += ms; return; }
    pf_hist_add(&r->all, ms); if (!kind || !kind[0]) kind = "-";
    int k = -1; for (int i = 0; i < r->nk; i++) if (!strcmp(r->kname[i], kind)) { k = i; break; }
    if (k < 0) { if (r->nk < PF_KINDS) { k = r->nk++; strncpy(r->kname[k], kind, sizeof r->kname[k] - 1); } else k = PF_KINDS - 1; }   // table full: the last slot collects the rest
    pf_hist_add(&r->kind[k], ms);
}
static void pf_rec_merge(PfRec *d, const PfRec *s) {
    pf_hist_merge(&d->all, &s->all); d->stalls += s->stalls; d->stall_ms += s->stall_ms;
    for (int i = 0; i < s->nk; i++) { const char *kn = s->kname[i]; int k = -1; for (int j = 0; j < d->nk; j++) if (!strcmp(d->kname[j], kn)) { k = j; break; }
        if (k < 0) { if (d->nk < PF_KINDS) { k = d->nk++; strncpy(d->kname[k], kn, sizeof d->kname[k] - 1); } else k = PF_KINDS - 1; }
        pf_hist_merge(&d->kind[k], &s->kind[i]); }
}
// one log line for a histogram: "<label>: 1234 frames, avg 9.8 ms (102 fps), 1% low 14.2 ms (70 fps), 0.1% low 31 ms (32 fps), max 48 ms | <8:12 8-16:1100 ..."
static int pf_line(const PfHist *h, const char *label, char *out, size_t cap) {
    unsigned b[7]; pf_bands(h, b); double avg = h->n ? h->sum / h->n : 0, l1 = pf_low_ms(h, 0.01), l01 = pf_low_ms(h, 0.001);
    return snprintf(out, cap, "%s: %u frames, avg %.1f ms (%.0f fps), 1%% low %.1f ms (%.0f fps), 0.1%% low %.1f ms (%.0f fps), max %.0f ms | <8:%u 8-16:%u 16-25:%u 25-33:%u 33-50:%u 50-100:%u >=100:%u",
        label, h->n, avg, avg > 0 ? 1000 / avg : 0, l1, l1 > 0 ? 1000 / l1 : 0, l01, l01 > 0 ? 1000 / l01 : 0, h->max, b[0], b[1], b[2], b[3], b[4], b[5], b[6]);
}
#endif
