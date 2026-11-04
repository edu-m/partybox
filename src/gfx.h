#pragma once
#include "efi.h"

/* Colors are 0x00RRGGBB, which matches EFI_GRAPHICS_OUTPUT_BLT_PIXEL in memory. */
typedef uint32_t color_t;

struct glyph {
    uint16_t cp;
    uint8_t w, h;
    int8_t bx;      /* left bearing */
    int16_t by;     /* offset from top of line box */
    uint8_t adv;
    uint32_t off;   /* offset into nibble data, in pixels */
};

struct font {
    int line_h, ascent, count;
    const struct glyph *glyphs;
    const uint8_t *data;
};

struct fonts {
    const struct font *small, *body, *title;
    int scale; /* 1 or 2, for spacing */
};

struct gfx {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    uint32_t w, h;
    color_t *back;
    struct fonts f;
    int cx, cy, cw, ch; /* clip rect */
    uint32_t mode0;     /* GOP mode partybox started in */
};

EFI_STATUS gfx_init(struct gfx *g);   /* (re)query mode, (re)alloc back buffer */
void gfx_reclaim(struct gfx *g);      /* after a child app ran: restore our mode */
void gfx_present(struct gfx *g, int x, int y, int w, int h);
void gfx_present_all(struct gfx *g);

void gfx_clip(struct gfx *g, int x, int y, int w, int h);
void gfx_noclip(struct gfx *g);

void gfx_fill(struct gfx *g, int x, int y, int w, int h, color_t c);
void gfx_vgradient(struct gfx *g, int x, int y, int w, int h, color_t top, color_t bottom);
void gfx_round_rect(struct gfx *g, int x, int y, int w, int h, int r, color_t c);
void gfx_dim(struct gfx *g, int x, int y, int w, int h, color_t c, unsigned a); /* blend c over region */

/* Text. Returns the x after the last glyph. max_w > 0 truncates with an ellipsis. */
int gfx_text(struct gfx *g, const struct font *f, int x, int y, const CHAR16 *s, color_t c, int max_w);
int gfx_text_width(const struct font *f, const CHAR16 *s);
/* Metrics of the glyph that would be drawn for cp (falls back like gfx_text). */
const struct glyph *gfx_glyph(const struct font *f, CHAR16 cp);
/* Word-wrapped text within width w. g == NULL only measures. Returns the height used. */
int gfx_text_wrap(struct gfx *g, const struct font *f, int x, int y, int w, const CHAR16 *s, color_t c);

color_t gfx_mix(color_t a, color_t b, unsigned t /* 0..255, weight of b */);
