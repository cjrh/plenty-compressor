/* Experimental LV2 ABI adapter. See README before using in a host. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include "compressor.h"

/* Minimal LV2 core ABI, expressed locally to avoid a build dependency. */
typedef void *Handle;
typedef struct { const char *URI; void *data; } Feature;
typedef struct Descriptor Descriptor;
struct Descriptor {
    const char *URI;
    Handle (*instantiate)(const Descriptor *, double, const char *, const Feature *const *);
    void (*connect_port)(Handle, uint32_t, void *);
    void (*activate)(Handle);
    void (*run)(Handle, uint32_t);
    void (*deactivate)(Handle);
    void (*cleanup)(Handle);
    const void *(*extension_data)(const char *);
};

enum { IN_L, IN_R, OUT_L, OUT_R, THRESHOLD, RATIO, ATTACK, RELEASE, PORTS };
typedef struct { float *ports[PORTS]; double rate, envelope; } Compressor;

static Handle instantiate(const Descriptor *d, double rate, const char *path, const Feature *const *features) {
    (void)d; (void)path; (void)features;
    if (!isfinite(rate) || rate <= 0) return NULL;
    Compressor *c = calloc(1, sizeof(*c));
    if (c) c->rate = rate;
    return c;
}
static void connect_port(Handle instance, uint32_t port, void *data) {
    Compressor *c = instance;
    if (port < PORTS) c->ports[port] = data;
}
static void activate(Handle instance) { ((Compressor *)instance)->envelope = 0; }
static double control(const float *p, double fallback, double low, double high) {
    double x = p ? *p : fallback;
    if (!isfinite(x)) return fallback;
    return x < low ? low : (x > high ? high : x);
}
static void run(Handle instance, uint32_t n) {
    Compressor *c = instance;
    float **p = c->ports;
    if (!p[IN_L] || !p[IN_R] || !p[OUT_L] || !p[OUT_R]) return;
    double threshold = pow(10.0, control(p[THRESHOLD], -18, -60, 0) / 20.0);
    double ratio = control(p[RATIO], 4, 1, 20);
    double attack = exp(-1.0 / (0.001 * c->rate * control(p[ATTACK], 10, 0.1, 200)));
    double release = exp(-1.0 / (0.001 * c->rate * control(p[RELEASE], 100, 1, 2000)));
    for (uint32_t i = 0; i < n; ++i) {
        double l = isfinite(p[IN_L][i]) ? p[IN_L][i] : 0.0;
        double r = isfinite(p[IN_R][i]) ? p[IN_R][i] : 0.0;
        double gain = compressor_gain(l, r, &c->envelope, threshold, ratio, attack, release);
        p[OUT_L][i] = (float)(l * gain);
        p[OUT_R][i] = (float)(r * gain);
    }
}
static void cleanup(Handle instance) { free(instance); }
static const Descriptor descriptor = {"https://example.org/plenty-compressor", instantiate, connect_port, activate, run, NULL, cleanup, NULL};
__attribute__((visibility("default"))) const Descriptor *lv2_descriptor(uint32_t index) {
    return index == 0 ? &descriptor : NULL;
}
