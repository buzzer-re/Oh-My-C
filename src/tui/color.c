/* color.c - OKLCH / HSV color math and session accents (port of omp utils/color.ts and
 * tui/theme/session-color.ts). */
#include "tui/color.h"

#include <math.h>

static double lin(int ch) {
    double c = ch / 255.0;
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double delin(double l) {
    double c = l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1 / 2.4) - 0.055;
    return c * 255;
}

static int clamp255(double v) {
    v = v < 0 ? 0 : v > 255 ? 255 : v;
    return (int)floor(v + 0.5); /* JS Math.round for non-negative values */
}

static void lab_to_lin(double L, double a, double b, double *r, double *g, double *bl) {
    double l = pow(L + 0.3963377774 * a + 0.2158037573 * b, 3);
    double m = pow(L - 0.1055613458 * a - 0.0638541728 * b, 3);
    double s = pow(L - 0.0894841775 * a - 1.291485548 * b, 3);
    *r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    *g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    *bl = -0.0041960863 * l - 0.7034186147 * m + 1.707614701 * s;
}

tui_oklch_t tui_rgb_to_oklch(uint32_t rgb) {
    double r = lin((rgb >> 16) & 255), g = lin((rgb >> 8) & 255), b = lin(rgb & 255);
    double l = cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    double m = cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    double s = cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    double L = 0.2104542553 * l + 0.793617785 * m - 0.0040720468 * s;
    double A = 1.9779984951 * l - 2.428592205 * m + 0.4505937099 * s;
    double B = 0.0259040371 * l + 0.7827717662 * m - 0.808675766 * s;
    tui_oklch_t o = {L, hypot(A, B), atan2(B, A) * 180 / M_PI};
    if (o.h < 0) o.h += 360;
    return o;
}

static double max_saturation(double a, double b) {
    double k0, k1, k2, k3, k4, wl, wm, ws;
    if (-1.88170328 * a - 0.80936493 * b > 1) {
        k0 = 1.19086277, k1 = 1.76576728, k2 = 0.59662641, k3 = 0.75515197, k4 = 0.56771245;
        wl = 4.0767416621, wm = -3.3077115913, ws = 0.2309699292;
    } else if (1.81444104 * a - 1.19445276 * b > 1) {
        k0 = 0.73956515, k1 = -0.45954404, k2 = 0.08285427, k3 = 0.1254107, k4 = 0.14503204;
        wl = -1.2684380046, wm = 2.6097574011, ws = -0.3413193965;
    } else {
        k0 = 1.35733652, k1 = -0.00915799, k2 = -1.1513021, k3 = -0.50559606, k4 = 0.00692167;
        wl = -0.0041960863, wm = -0.7034186147, ws = 1.707614701;
    }
    double S = k0 + k1 * a + k2 * b + k3 * a * a + k4 * a * b;
    double kl = 0.3963377774 * a + 0.2158037573 * b;
    double km = -0.1055613458 * a - 0.0638541728 * b;
    double ks = -0.0894841775 * a - 1.291485548 * b;
    double l_ = 1 + S * kl, m_ = 1 + S * km, s_ = 1 + S * ks;
    double f = wl * l_ * l_ * l_ + wm * m_ * m_ * m_ + ws * s_ * s_ * s_;
    double f1 = wl * 3 * kl * l_ * l_ + wm * 3 * km * m_ * m_ + ws * 3 * ks * s_ * s_;
    double f2 = wl * 6 * kl * kl * l_ + wm * 6 * km * km * m_ + ws * 6 * ks * ks * s_;
    return S - (f * f1) / (f1 * f1 - 0.5 * f * f2);
}

void tui_oklch_cusp(double h, double *l, double *c) {
    double hr = h * M_PI / 180, a = cos(hr), b = sin(hr);
    double s = max_saturation(a, b);
    double r, g, bl;
    lab_to_lin(1, s * a, s * b, &r, &g, &bl);
    double mx = MAX(r, MAX(g, bl));
    *l = cbrt(1 / mx);
    *c = *l * s;
}

double tui_hue_distance(double a, double b) {
    double d = fmod(fabs(a - b), 360);
    return MIN(d, 360 - d);
}

static bool in_gamut(double r, double g, double b) {
    const double e = 1e-4;
    return r >= -e && r <= 1 + e && g >= -e && g <= 1 + e && b >= -e && b <= 1 + e;
}

uint32_t tui_oklch_to_rgb(tui_oklch_t o) {
    double l = o.l < 0 ? 0 : o.l > 1 ? 1 : o.l;
    double hr = o.h * M_PI / 180, cs = cos(hr), sn = sin(hr);
    double r, g, b;
    lab_to_lin(l, o.c * cs, o.c * sn, &r, &g, &b);
    if (!in_gamut(r, g, b)) {
        double lo = 0, hi = o.c;
        for (int i = 0; i < 20; i++) {
            double mid = (lo + hi) / 2;
            lab_to_lin(l, mid * cs, mid * sn, &r, &g, &b);
            if (in_gamut(r, g, b)) lo = mid;
            else hi = mid;
        }
        lab_to_lin(l, lo * cs, lo * sn, &r, &g, &b);
    }
    return (uint32_t)(clamp255(delin(r)) << 16 | clamp255(delin(g)) << 8 | clamp255(delin(b)));
}

uint32_t tui_adjust_hsv(uint32_t rgb, double dh, double ms, double mv) {
    double r = ((rgb >> 16) & 255) / 255.0, g = ((rgb >> 8) & 255) / 255.0, b = (rgb & 255) / 255.0;
    double mx = MAX(r, MAX(g, b)), mn = MIN(r, MIN(g, b)), d = mx - mn, h = 0;
    if (d != 0) {
        if (mx == r) h = ((g - b) / d + (g < b ? 6 : 0)) / 6;
        else if (mx == g) h = ((b - r) / d + 2) / 6;
        else h = ((r - g) / d + 4) / 6;
    }
    h *= 360;
    double s = mx == 0 ? 0 : d / mx, v = mx;
    h = fmod(h + dh, 360);
    if (h < 0) h += 360;
    s = MAX(0.0, MIN(1.0, s * ms));
    v = MAX(0.0, MIN(1.0, v * mv));
    double hh = fmod(fmod(h, 360) + 360, 360);
    int i = (int)floor(hh / 60);
    double f = hh / 60 - i, p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    double R, G, B;
    switch (i % 6) {
    case 0: R = v, G = t, B = p; break;
    case 1: R = q, G = v, B = p; break;
    case 2: R = p, G = v, B = t; break;
    case 3: R = p, G = q, B = v; break;
    case 4: R = t, G = p, B = v; break;
    default: R = v, G = p, B = q; break;
    }
    return (uint32_t)(clamp255(R * 255) << 16 | clamp255(G * 255) << 8 | clamp255(B * 255));
}

double tui_color_luma(uint32_t rgb) {
    return (0.2126 * ((rgb >> 16) & 255) + 0.7152 * ((rgb >> 8) & 255) + 0.0722 * (rgb & 255)) / 255;
}

double tui_relative_luminance(uint32_t rgb) {
    return 0.2126 * lin((rgb >> 16) & 255) + 0.7152 * lin((rgb >> 8) & 255) + 0.0722 * lin(rgb & 255);
}

uint32_t tui_palette_rgb(int index) {
    static const uint32_t ANSI16[16] = {0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xc0c0c0,
                                        0x808080, 0xff0000, 0x00ff00, 0xffff00, 0x0000ff, 0xff00ff, 0x00ffff, 0xffffff};
    static const int STEPS[6] = {0, 95, 135, 175, 215, 255};
    if (index < 0 || index > 255) return 0;
    if (index < 16) return ANSI16[index];
    if (index < 232) {
        int n = index - 16;
        return (uint32_t)(STEPS[(n / 36) % 6] << 16 | STEPS[(n / 6) % 6] << 8 | STEPS[n % 6]);
    }
    int gr = 8 + (index - 232) * 10;
    return (uint32_t)(gr << 16 | gr << 8 | gr);
}

bool tui_theme_is_light(const tui_theme_t *th) {
    if (!th) th = tui_theme();
    int64_t bg = th->rgb[TUI_TC_STATUS_LINE_BG];
    return bg >= 0 && tui_color_luma((uint32_t)bg) > 0.5;
}

uint32_t tui_theme_rgb(const tui_theme_t *th, tui_tc_t slot) {
    if (!th) th = tui_theme();
    int64_t v = th->rgb[slot];
    if (v >= 0) return (uint32_t)v;
    return tui_theme_is_light(th) ? 0x000000 : 0xe5e5e7;
}

const char *tui_rgb_fg(const tui_theme_t *th, uint32_t rgb, char *buf, size_t n) {
    if (!th) th = tui_theme();
    int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    if (th->mode == TUI_COLOR_TRUECOLOR) snprintf(buf, n, "\x1b[38;2;%d;%d;%dm", r, g, b);
    else snprintf(buf, n, "\x1b[38;5;%dm", tui_rgb_to_256(r, g, b));
    return buf;
}

/* ---------- session accent ---------- */

#define MIN_CHROMA 0.05
#define MAX_CHROMA 0.21
#define DARK_MIN_L 0.65
#define DARK_MAX_L 0.88
#define MIN_HUE_DIST 10.0
#define MIN_CHROMA_FOR_HUE 0.03

typedef struct {
    int a, b;
} hue_iv_t;

static hue_iv_t g_dark_iv[32];
static int g_dark_n = -1;

static void dark_intervals(void) {
    if (g_dark_n >= 0) return;
    int n = 0, start = -1;
    for (int h = 0; h <= 360; h++) {
        double l = 2, c;
        if (h < 360) tui_oklch_cusp(h, &l, &c);
        if (h < 360 && l <= DARK_MAX_L) {
            if (start < 0) start = h;
        } else if (start >= 0) {
            if (n < 32) g_dark_iv[n++] = (hue_iv_t){start, h - 1};
            start = -1;
        }
    }
    g_dark_n = n;
}

static int arc_len(const hue_iv_t *iv, int n) {
    int t = 0;
    for (int i = 0; i < n; i++) t += iv[i].b - iv[i].a + 1;
    return t;
}

static double arc_hue(const hue_iv_t *iv, int n, int pos) {
    int p = pos;
    for (int i = 0; i < n; i++) {
        int w = iv[i].b - iv[i].a + 1;
        if (p < w) return iv[i].a + p;
        p -= w;
    }
    return n ? iv[n - 1].b : 0;
}

static uint32_t name_hash(const char *name) {
    uint32_t h = 5381;
    const unsigned char *s = (const unsigned char *)name;
    size_t len = strlen(name), i = 0;
    while (i < len) {
        uint32_t cp;
        int k = utf8_decode((const char *)s + i, len - i, &cp);
        if (k <= 0) k = 1, cp = s[i];
        i += (size_t)k;
        if (cp > 0xFFFF) { /* JS charCodeAt walks UTF-16 code units */
            cp -= 0x10000;
            h = ((h << 5) + h) ^ (0xD800 + (cp >> 10));
            h = ((h << 5) + h) ^ (0xDC00 + (cp & 0x3FF));
        } else {
            h = ((h << 5) + h) ^ cp;
        }
    }
    return h;
}

uint32_t tui_session_accent(const char *name, const tui_theme_t *th) {
    if (!th) th = tui_theme();
    static const tui_tc_t MAJORS[] = {TUI_TC_ACCENT,        TUI_TC_BORDER,           TUI_TC_BORDER_ACCENT,  TUI_TC_BORDER_MUTED,
                                      TUI_TC_SUCCESS,       TUI_TC_ERROR,            TUI_TC_WARNING,        TUI_TC_MD_HEADING,
                                      TUI_TC_MD_LINK,       TUI_TC_MD_CODE,          TUI_TC_MD_CODE_BLOCK,  TUI_TC_MD_QUOTE_BORDER,
                                      TUI_TC_MD_LIST_BULLET, TUI_TC_TOOL_DIFF_ADDED, TUI_TC_TOOL_DIFF_REMOVED,
                                      TUI_TC_CUSTOM_MESSAGE_LABEL, TUI_TC_THINKING_TEXT};
    bool light = tui_theme_is_light(th);
    static const hue_iv_t LIGHT_IV[1] = {{195, 330}};
    const hue_iv_t *iv;
    int niv;
    if (light) {
        iv = LIGHT_IV, niv = 1;
    } else {
        dark_intervals();
        iv = g_dark_iv, niv = g_dark_n;
    }
    int total = arc_len(iv, niv);
    if (total <= 0) return tui_theme_rgb(th, TUI_TC_ACCENT);
    int pos = (int)(name_hash(name) % (uint32_t)total);
    double occupied[ARRAY_LEN(MAJORS)];
    int nocc = 0;
    for (size_t i = 0; i < ARRAY_LEN(MAJORS); i++) {
        tui_oklch_t o = tui_rgb_to_oklch(tui_theme_rgb(th, MAJORS[i]));
        if (isfinite(o.h) && o.c >= MIN_CHROMA_FOR_HUE) occupied[nocc++] = o.h;
    }
#define HUE_AT(p) arc_hue(iv, niv, MAX(0, MIN(total - 1, (p))))
#define SAFE(cand)                                                                     \
    ({                                                                                 \
        bool ok_ = true;                                                               \
        for (int k_ = 0; k_ < nocc; k_++)                                              \
            if (tui_hue_distance((cand), occupied[k_]) < MIN_HUE_DIST) ok_ = false;    \
        ok_;                                                                           \
    })
    double target = HUE_AT(pos), hue = target;
    if (nocc && !SAFE(target)) {
        bool found = false;
        for (int d = 1; d < total && !found; d++)
            for (int dir = 1; dir >= -1 && !found; dir -= 2) {
                double cand = HUE_AT(pos + d * dir);
                if (SAFE(cand)) hue = cand, found = true;
            }
    }
#undef SAFE
#undef HUE_AT
    tui_oklch_t acc = tui_rgb_to_oklch(tui_theme_rgb(th, TUI_TC_ACCENT));
    bool usable = isfinite(acc.l) && isfinite(acc.c) && isfinite(acc.h) && acc.c >= MIN_CHROMA_FOR_HUE;
    double tl, tc;
    tui_oklch_cusp(hue, &tl, &tc);
    double lightness, frac;
    if (usable) {
        double sl, sc;
        tui_oklch_cusp(acc.h, &sl, &sc);
        lightness = acc.l <= sl ? (acc.l / sl) * tl : tl + ((acc.l - sl) / (1 - sl)) * (1 - tl);
        frac = acc.c / sc;
    } else {
        lightness = tl;
        frac = 0.7;
    }
    double chroma = MAX(MIN_CHROMA, MIN(MAX_CHROMA, frac * tc));
    if (!light) {
        double l = MAX(DARK_MIN_L, MIN(DARK_MAX_L, lightness));
        return tui_oklch_to_rgb((tui_oklch_t){l, chroma, hue});
    }
    double surf = th->rgb[TUI_TC_STATUS_LINE_BG] >= 0 ? tui_relative_luminance((uint32_t)th->rgb[TUI_TC_STATUS_LINE_BG]) : 1;
    double cap = MAX(0.0, (surf + 0.05) / 3 - 0.05);
    uint32_t top = tui_oklch_to_rgb((tui_oklch_t){lightness, chroma, hue});
    if (tui_relative_luminance(top) <= cap) return top;
    double lo = 0, hi = lightness;
    for (int i = 0; i < 20; i++) {
        double mid = (lo + hi) / 2;
        if (tui_relative_luminance(tui_oklch_to_rgb((tui_oklch_t){mid, chroma, hue})) > cap) hi = mid;
        else lo = mid;
    }
    return tui_oklch_to_rgb((tui_oklch_t){lo, chroma, hue});
}
