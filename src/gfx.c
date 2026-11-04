#include "gfx.h"
#include "lib.h"
#include "font_data.h"

/* Use a GOP that belongs to real hardware (its handle has a device path).
 * Firmware console splitters expose a virtual GOP too, but when Limine or a
 * child app sets a mode on the device GOP directly, the splitter's idea of the
 * resolution goes stale -- so draw where everyone else does. */
static EFI_GRAPHICS_OUTPUT_PROTOCOL *find_gop(void) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_HANDLE *hs = NULL;
    UINTN n = 0;
    if (!EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &gEfiGraphicsOutputProtocolGuid, NULL, &n, &hs))) {
        for (UINTN i = 0; i < n && !gop; i++) {
            void *dp = NULL;
            if (EFI_ERROR(gBS->HandleProtocol(hs[i], &gEfiDevicePathProtocolGuid, &dp)) || !dp) continue;
            if (EFI_ERROR(gBS->HandleProtocol(hs[i], &gEfiGraphicsOutputProtocolGuid, (void **)&gop))) gop = NULL;
        }
        xfree(hs);
    }
    if (!gop && EFI_ERROR(gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL, (void **)&gop))) gop = NULL;
    if (!gop && gST->ConsoleOutHandle &&
        EFI_ERROR(gBS->HandleProtocol(gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (void **)&gop)))
        gop = NULL;
    return gop;
}

EFI_STATUS gfx_init(struct gfx *g) {
    if (!g->gop) {
        g->gop = find_gop();
        if (!g->gop) return EFI_UNSUPPORTED;
        g->mode0 = g->gop->Mode->Mode;
    }

    uint32_t w = g->gop->Mode->Info->HorizontalResolution;
    uint32_t h = g->gop->Mode->Info->VerticalResolution;
    if (!g->back || w != g->w || h != g->h) {
        xfree(g->back);
        g->back = xalloc((size_t)w * h * sizeof(color_t));
        if (!g->back) return EFI_OUT_OF_RESOURCES;
        g->w = w;
        g->h = h;
    }
    gfx_noclip(g);

    if (h >= 1400 && w >= 2000) {
        g->f = (struct fonts){ &font_small_2x, &font_body_2x, &font_title_2x, 2 };
    } else {
        g->f = (struct fonts){ &font_small_1x, &font_body_1x, &font_title_1x, 1 };
    }
    return EFI_SUCCESS;
}

void gfx_clip(struct gfx *g, int x, int y, int w, int h) {
    g->cx = x; g->cy = y; g->cw = w; g->ch = h;
}

void gfx_noclip(struct gfx *g) {
    gfx_clip(g, 0, 0, (int)g->w, (int)g->h);
}

/* A child app may have switched modes; put ours back. */
void gfx_reclaim(struct gfx *g) {
    if (!g->gop) return;
    if (g->gop->Mode->Mode != g->mode0) g->gop->SetMode(g->gop, g->mode0);
    gfx_init(g);
}

static bool clip_to(int cx, int cy, int cw, int ch, int *x, int *y, int *w, int *h) {
    if (*x < cx) { *w -= cx - *x; *x = cx; }
    if (*y < cy) { *h -= cy - *y; *y = cy; }
    if (*x + *w > cx + cw) *w = cx + cw - *x;
    if (*y + *h > cy + ch) *h = cy + ch - *y;
    return *w > 0 && *h > 0;
}

static bool clip(struct gfx *g, int *x, int *y, int *w, int *h) {
    return clip_to(g->cx, g->cy, g->cw, g->ch, x, y, w, h);
}

void gfx_present(struct gfx *g, int x, int y, int w, int h) {
    if (!clip_to(0, 0, (int)g->w, (int)g->h, &x, &y, &w, &h)) return;
    g->gop->Blt(g->gop, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)g->back, EfiBltBufferToVideo,
                (UINTN)x, (UINTN)y, (UINTN)x, (UINTN)y, (UINTN)w, (UINTN)h,
                g->w * sizeof(color_t));
}

void gfx_present_all(struct gfx *g) {
    gfx_present(g, 0, 0, (int)g->w, (int)g->h);
}

color_t gfx_mix(color_t a, color_t b, unsigned t) {
    unsigned it = 255 - t;
    unsigned r = (((a >> 16) & 255) * it + ((b >> 16) & 255) * t + 127) / 255;
    unsigned gr = (((a >> 8) & 255) * it + ((b >> 8) & 255) * t + 127) / 255;
    unsigned bl = ((a & 255) * it + (b & 255) * t + 127) / 255;
    return (r << 16) | (gr << 8) | bl;
}

void gfx_fill(struct gfx *g, int x, int y, int w, int h, color_t c) {
    if (!clip(g, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        color_t *row = g->back + (size_t)(y + j) * g->w + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

void gfx_vgradient(struct gfx *g, int x, int y, int w, int h, color_t top, color_t bottom) {
    int h0 = h > 1 ? h - 1 : 1, y0 = y;
    if (!clip(g, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        color_t c = gfx_mix(top, bottom, (unsigned)((y + j - y0) * 255 / h0));
        color_t *row = g->back + (size_t)(y + j) * g->w + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

void gfx_dim(struct gfx *g, int x, int y, int w, int h, color_t c, unsigned a) {
    if (!clip(g, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        color_t *row = g->back + (size_t)(y + j) * g->w + x;
        for (int i = 0; i < w; i++) row[i] = gfx_mix(row[i], c, a);
    }
}

static inline void blend_px(struct gfx *g, int x, int y, color_t c, unsigned a) {
    if (x < g->cx || y < g->cy || x >= g->cx + g->cw || y >= g->cy + g->ch || !a) return;
    color_t *p = g->back + (size_t)y * g->w + x;
    *p = a >= 255 ? c : gfx_mix(*p, c, a);
}

/* Rounded rectangle with 4x4 supersampled corners. */
void gfx_round_rect(struct gfx *g, int x, int y, int w, int h, int r, color_t c) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    if (r <= 0) { gfx_fill(g, x, y, w, h, c); return; }

    gfx_fill(g, x + r, y, w - 2 * r, h, c);
    gfx_fill(g, x, y + r, r, h - 2 * r, c);
    gfx_fill(g, x + w - r, y + r, r, h - 2 * r, c);

    int R = r * 4; /* radius in subpixels */
    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            int cov = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int dx = R - (i * 4 + sx) - 1, dy = R - (j * 4 + sy) - 1;
                    /* subpixel centers: 2*d+1 against 2*R */
                    int ddx = 2 * dx + 1, ddy = 2 * dy + 1;
                    if (ddx * ddx + ddy * ddy <= 4 * R * R) cov++;
                }
            if (!cov) continue;
            unsigned a = (unsigned)(cov * 255 / 16);
            blend_px(g, x + i, y + j, c, a);                 /* top-left */
            blend_px(g, x + w - 1 - i, y + j, c, a);         /* top-right */
            blend_px(g, x + i, y + h - 1 - j, c, a);         /* bottom-left */
            blend_px(g, x + w - 1 - i, y + h - 1 - j, c, a); /* bottom-right */
        }
    }
}

/* UI fonts store ASCII first, in order, so most lookups are direct; small
 * special-purpose fonts (e.g. card suits) hold arbitrary glyph sets. */
static const struct glyph *lookup(const struct font *f, CHAR16 cp) {
    if (cp >= 32 && cp < 127 && cp - 32 < f->count && f->glyphs[cp - 32].cp == cp)
        return &f->glyphs[cp - 32];
    for (int i = 0; i < f->count; i++)
        if (f->glyphs[i].cp == cp) return &f->glyphs[i];
    return NULL;
}

static const struct glyph *find_glyph(const struct font *f, CHAR16 cp) {
    const struct glyph *g = lookup(f, cp);
    if (!g) g = lookup(f, '?');
    return g ? g : &f->glyphs[0];
}

const struct glyph *gfx_glyph(const struct font *f, CHAR16 cp) {
    return find_glyph(f, cp);
}

static void draw_glyph(struct gfx *g, const struct font *f, const struct glyph *gl, int x, int y, color_t c) {
    uint32_t idx = gl->off;
    for (int j = 0; j < gl->h; j++) {
        for (int i = 0; i < gl->w; i++, idx++) {
            unsigned a = (f->data[idx >> 1] >> ((idx & 1) * 4)) & 15;
            if (a) blend_px(g, x + gl->bx + i, y + gl->by + j, c, a * 17);
        }
    }
}

int gfx_text_width(const struct font *f, const CHAR16 *s) {
    int w = 0;
    for (; s && *s; s++) w += find_glyph(f, *s)->adv;
    return w;
}

int gfx_text(struct gfx *g, const struct font *f, int x, int y, const CHAR16 *s, color_t c, int max_w) {
    const struct glyph *ell = find_glyph(f, 0x2026);
    bool truncate = max_w > 0 && gfx_text_width(f, s) > max_w;
    int limit = x + max_w - (truncate ? ell->adv : 0);
    for (; s && *s; s++) {
        const struct glyph *gl = find_glyph(f, *s);
        if (truncate && x + gl->adv > limit) {
            draw_glyph(g, f, ell, x, y, c);
            return x + ell->adv;
        }
        draw_glyph(g, f, gl, x, y, c);
        x += gl->adv;
    }
    return x;
}

int gfx_text_wrap(struct gfx *g, const struct font *f, int x, int y, int w, const CHAR16 *s, color_t c) {
    int cx = x, cy = y, space = find_glyph(f, ' ')->adv;
    while (s && *s) {
        const CHAR16 *e = s;
        int ww = 0;
        while (*e && *e != ' ') ww += find_glyph(f, *e++)->adv;
        if (cx > x && cx + ww > x + w) { cx = x; cy += f->line_h; }
        for (; s < e; s++) {
            const struct glyph *gl = find_glyph(f, *s);
            if (cx > x && cx + gl->adv > x + w) { cx = x; cy += f->line_h; } /* break overlong words */
            if (g) draw_glyph(g, f, gl, cx, cy, c);
            cx += gl->adv;
        }
        while (*s == ' ') { s++; cx += space; }
    }
    return cy + f->line_h - y;
}
