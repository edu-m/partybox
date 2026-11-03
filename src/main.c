/* partybox: a tiny chainloadable EFI launcher for whatever is in .\PARTYBOX */
#include "lib.h"
#include "gfx.h"
#include "scan.h"

#ifndef PARTYBOX_VERSION        /* normally injected from VERSION by the Makefile */
#define PARTYBOX_VERSION L"dev"
#endif
#define DEFAULT_TIMEOUT  5   /* seconds; override with "timeout=N" in the load options */

EFI_HANDLE gImageHandle;
EFI_SYSTEM_TABLE *gST;
EFI_BOOT_SERVICES *gBS;
EFI_RUNTIME_SERVICES *gRT;

/* ---- palette ---------------------------------------------------------- */
#define C_BG_TOP   0x1b1729
#define C_BG_BOT   0x0c0a12
#define C_PANEL    0x1f1b2e
#define C_TEXT     0xeeeaf6
#define C_DIM      0x8d87a6
#define C_FAINT    0x5d5873
#define C_LABEL    0xa48fd0
#define C_ACCENT   0xff5c8a
#define C_SEL_TEXT 0xffffff
#define C_SEL_DIM  0xffd5e2
#define C_OK       0x7bd88f
#define C_ERR      0xff6b6b
#define C_WARN     0xffb547
#define C_WARN_INK 0x2a1c05
#define C_DIALOG   0x29243b
#define C_BUTTON   0x3a344f

/* ---- menu model -------------------------------------------------------- */
enum row_kind { ROW_LABEL, ROW_SPACER, ROW_NOTE, ROW_APP, ROW_REBOOT, ROW_FWUI, ROW_SHUTDOWN, ROW_EXIT };

struct row {
    enum row_kind kind;
    int app;               /* index into scan.apps for ROW_APP */
    const CHAR16 *text;    /* for labels, notes and system rows */
};

struct ui {
    struct gfx g;
    bool have_gfx;
    struct scan scan;
    struct row *rows;
    int nrows;
    int sel;               /* index into rows */
    int scroll;            /* pixels */
    bool fwui_supported;
    bool secure_boot;
    CHAR16 msg[256];
    color_t msg_color;
    int countdown;         /* seconds left before autoboot, 0 = off */
    EFI_EVENT timer;
};

static bool selectable(const struct row *r) {
    return r->kind >= ROW_APP;
}

static void build_rows(struct ui *u) {
    xfree(u->rows);
    int max = u->scan.count + 10;
    u->rows = xalloc(sizeof(struct row) * (size_t)max);
    int n = 0;
    if (!u->rows) { u->nrows = 0; return; }

    u->rows[n++] = (struct row){ ROW_LABEL, 0, L"APPLICATIONS" };
    if (!u->scan.found) {
        u->rows[n++] = (struct row){ ROW_NOTE, 0, L"No PARTYBOX folder next to partybox.efi (or in \\PARTYBOX on any volume)" };
    } else if (!u->scan.count) {
        u->rows[n++] = (struct row){ ROW_NOTE, 0, L"Nothing here yet. Drop some .efi files in and press R" };
    }
    for (int i = 0; i < u->scan.count; i++) u->rows[n++] = (struct row){ ROW_APP, i, NULL };

    u->rows[n++] = (struct row){ ROW_SPACER, 0, NULL };
    u->rows[n++] = (struct row){ ROW_LABEL, 0, L"SYSTEM" };
    u->rows[n++] = (struct row){ ROW_REBOOT, 0, L"Reboot" };
    u->rows[n++] = (struct row){ ROW_FWUI, 0, L"Firmware Settings" };
    u->rows[n++] = (struct row){ ROW_SHUTDOWN, 0, L"Shut Down" };
    u->rows[n++] = (struct row){ ROW_EXIT, 0, L"Exit" };
    u->nrows = n;
}

/* Land on the first app that can actually start (that's what autoboot runs),
 * else on the first selectable row. */
static void select_first(struct ui *u) {
    for (int i = 0; i < u->nrows; i++) {
        if (u->rows[i].kind == ROW_APP && !u->scan.apps[u->rows[i].app].problem) {
            u->sel = i;
            return;
        }
    }
    for (u->sel = 0; u->sel < u->nrows && !selectable(&u->rows[u->sel]); u->sel++) {}
}

/* Rescan, trying to keep the same app selected. */
static void rescan(struct ui *u) {
    CHAR16 *prev = NULL;
    enum row_kind prev_kind = u->nrows ? u->rows[u->sel].kind : ROW_APP;
    if (u->nrows && prev_kind == ROW_APP) prev = wdup(u->scan.apps[u->rows[u->sel].app].rel);

    scan_run(&u->scan);
    build_rows(u);
    select_first(u);
    for (int i = 0; i < u->nrows; i++) {
        struct row *r = &u->rows[i];
        if ((prev && r->kind == ROW_APP && wicmp(u->scan.apps[r->app].rel, prev) == 0) ||
            (!prev && prev_kind != ROW_APP && r->kind == prev_kind)) {
            u->sel = i;
            break;
        }
    }
    xfree(prev);
}

static void move_sel(struct ui *u, int delta) {
    int i = u->sel;
    for (;;) {
        int next = i + (delta > 0 ? 1 : -1);
        if (next < 0 || next >= u->nrows) break;
        i = next;
        if (selectable(&u->rows[i]) && (delta > 0 ? --delta : ++delta) == 0) break;
    }
    if (selectable(&u->rows[i])) u->sel = i;
}

/* ---- firmware variables ---------------------------------------------- */
static bool get_u64_var(CHAR16 *name, uint64_t *out) {
    UINTN size = sizeof(*out);
    *out = 0;
    return !EFI_ERROR(gRT->GetVariable(name, &gEfiGlobalVariableGuid, NULL, &size, out));
}

static void probe_firmware(struct ui *u) {
    uint64_t sup = 0;
    u->fwui_supported = get_u64_var(L"OsIndicationsSupported", &sup) &&
                        (sup & EFI_OS_INDICATIONS_BOOT_TO_FW_UI);
    uint8_t sb = 0;
    UINTN size = 1;
    u->secure_boot = !EFI_ERROR(gRT->GetVariable(L"SecureBoot", &gEfiGlobalVariableGuid, NULL, &size, &sb)) && sb;
}

static void set_msg(struct ui *u, color_t c, const CHAR16 *a, const CHAR16 *b, const CHAR16 *d) {
    u->msg_color = c;
    wcopy(u->msg, 256, a);
    wappend(u->msg, 256, b);
    wappend(u->msg, 256, d);
}

static void append_status(CHAR16 *buf, size_t cap, EFI_STATUS st) {
    const CHAR16 *s = status_str(st);
    if (s) wappend(buf, cap, s);
    else wappend_hex(buf, cap, st);
}

/* ---- rendering -------------------------------------------------------- */
struct layout {
    int S, x0, col_w;
    int panel_y, panel_h, view_y, view_h, view_x, view_w;
    int row_h, label_h, spacer_h;
    int stat_y, foot_y;
};

static int row_height(const struct layout *L, const struct row *r) {
    switch (r->kind) {
    case ROW_LABEL: return L->label_h;
    case ROW_SPACER: return L->spacer_h;
    default: return L->row_h;
    }
}

static void compute_layout(struct ui *u, struct layout *L, int top) {
    struct gfx *g = &u->g;
    int S = g->f.scale, W = (int)g->w, H = (int)g->h;
    L->S = S;
    L->col_w = W - 48 * S;
    if (L->col_w > 760 * S) L->col_w = 760 * S;
    L->x0 = (W - L->col_w) / 2;
    L->row_h = g->f.body->line_h + 16 * S;
    L->label_h = g->f.small->line_h + 10 * S;
    L->spacer_h = 10 * S;

    int bottom_pad = H / 28 > 12 * S ? H / 28 : 12 * S;
    L->foot_y = H - g->f.small->line_h - bottom_pad;
    L->stat_y = L->foot_y - g->f.small->line_h - 14 * S;

    int content = 0;
    for (int i = 0; i < u->nrows; i++) content += row_height(L, &u->rows[i]);
    int pad = 10 * S;
    int avail = L->stat_y - 14 * S - top;
    L->panel_y = top;
    L->panel_h = content + 2 * pad < avail ? content + 2 * pad : avail;
    if (L->panel_h < L->row_h + 2 * pad) L->panel_h = L->row_h + 2 * pad;
    L->view_x = L->x0 + pad;
    L->view_w = L->col_w - 2 * pad;
    L->view_y = top + pad;
    L->view_h = L->panel_h - 2 * pad;
}

/* Adjust u->scroll so the selected row (and a label right above it) is visible. */
static int ensure_visible(struct ui *u, const struct layout *L, int *content_out) {
    int y = 0, sel_y = 0, sel_h = 0, label_y = -1, content = 0;
    for (int i = 0; i < u->nrows; i++) {
        int h = row_height(L, &u->rows[i]);
        if (i == u->sel - 1 && u->rows[i].kind == ROW_LABEL) label_y = y;
        if (i == u->sel) { sel_y = y; sel_h = h; }
        y += h;
    }
    content = y;
    int top_needed = label_y >= 0 ? label_y : sel_y;
    if (top_needed < u->scroll) u->scroll = top_needed;
    if (sel_y + sel_h > u->scroll + L->view_h) u->scroll = sel_y + sel_h - L->view_h;
    if (u->scroll > content - L->view_h) u->scroll = content - L->view_h;
    if (u->scroll < 0) u->scroll = 0;
    *content_out = content;
    return u->scroll;
}

static void draw_confetti(struct gfx *g, int x, int y, int lh, int S) {
    static const struct { int dx, dy, sz; color_t c; } bits[] = {
        { 0, 12, 7, 0xffd166 }, { 12, 4, 5, 0x4cc9f0 }, { 20, 18, 6, C_ACCENT },
        { 31, 8, 5, 0x06d6a0 }, { 40, 20, 4, 0x9b5de5 }, { 48, 10, 6, 0xffd166 },
    };
    int base = y + lh / 2 - 16 * S;
    for (unsigned i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        int sz = bits[i].sz * S;
        gfx_round_rect(g, x + bits[i].dx * S, base + bits[i].dy * S, sz, sz, sz / 3, bits[i].c);
    }
}

/* Amber circle with a "!" in it, d pixels across. */
static void draw_warn_chip(struct gfx *g, const struct font *f, int x, int y, int d) {
    gfx_round_rect(g, x, y, d, d, d / 2, C_WARN);
    int w = gfx_text_width(f, L"!");
    gfx_text(g, f, x + (d - w) / 2, y + (d - f->line_h) / 2, L"!", C_WARN_INK, 0);
}

static bool flag_unsigned(const struct ui *u, const struct app *a) {
    return u->secure_boot && !a->problem && !a->is_signed;
}

static bool launchable_row(const struct ui *u, int i) {
    return u->rows[i].kind == ROW_APP && !u->scan.apps[u->rows[i].app].problem;
}

static void draw_row(struct ui *u, const struct layout *L, const struct row *r, int y, bool sel, bool scrollbar) {
    struct gfx *g = &u->g;
    int S = L->S, h = row_height(L, r);
    int tx = L->view_x + 14 * S;
    int right = L->view_x + L->view_w - 14 * S - (scrollbar ? 8 * S : 0);
    const struct font *fb = g->f.body, *fs = g->f.small;
    int ty = y + (h - fb->line_h) / 2;
    int sy = y + (h - fs->line_h) / 2;

    switch (r->kind) {
    case ROW_SPACER:
        return;
    case ROW_LABEL:
        gfx_text(g, fs, tx, y + h - fs->line_h - 2 * S, r->text, C_LABEL, 0);
        return;
    case ROW_NOTE:
        gfx_text(g, fb, tx, ty, r->text, C_DIM, right - tx);
        return;
    default:
        break;
    }

    if (sel) gfx_round_rect(g, L->view_x, y + 2 * S, L->view_w - (scrollbar ? 8 * S : 0), h - 4 * S, 8 * S, C_ACCENT);
    color_t fg = sel ? C_SEL_TEXT : C_TEXT;
    color_t dim = sel ? C_SEL_DIM : C_DIM;

    CHAR16 right_text[64] = { 0 };
    const CHAR16 *label = r->text;
    bool warn = false;
    if (r->kind == ROW_APP) {
        const struct app *a = &u->scan.apps[r->app];
        label = a->label;
        warn = flag_unsigned(u, a);
        if (a->problem) {
            wcopy(right_text, 64, a->problem); /* greyed out, with the reason */
            fg = dim;
        } else {
            wformat_size(right_text, 64, a->size);
        }
        if (a->group) {
            CHAR16 grp[80];
            wcopy(grp, 80, a->group);
            wappend(grp, 80, L" / ");
            tx = gfx_text(g, fb, tx, ty, grp, dim, (right - tx) / 2);
        }
    } else if (r->kind == ROW_FWUI && !u->fwui_supported) {
        wcopy(right_text, 64, L"not supported");
        fg = dim;
    }

    int rw = gfx_text_width(fs, right_text);
    int chip = warn ? fs->line_h - 2 * S : 0;
    int lx = gfx_text(g, fb, tx, ty, label, fg, right - tx - rw - 16 * S - (chip ? chip + 10 * S : 0));
    if (chip) draw_warn_chip(g, fs, lx + 10 * S, y + (h - chip) / 2, chip);
    if (rw) gfx_text(g, fs, right - rw, sy, right_text, dim, 0);
}

/* Right-aligned text ending at `right`, truncated to max_w. */
static void text_right(struct gfx *g, const struct font *f, int right, int y, const CHAR16 *s, color_t c, int max_w) {
    if (max_w < 40 * g->f.scale) return;
    int w = gfx_text_width(f, s);
    if (w > max_w) w = max_w;
    gfx_text(g, f, right - w, y, s, c, max_w);
}

/* Draws the menu into the back buffer (does not present). */
static void render(struct ui *u) {
    struct gfx *g = &u->g;
    const struct font *ft = g->f.title, *fs = g->f.small;
    int S = g->f.scale;

    gfx_noclip(g);
    gfx_vgradient(g, 0, 0, (int)g->w, (int)g->h, C_BG_TOP, C_BG_BOT);

    /* Header */
    struct layout L;
    compute_layout(u, &L, 0);
    int right = L.x0 + L.col_w;
    int y = (int)g->h / 14;
    if (y < 14 * S) y = 14 * S;
    int tx = gfx_text(g, ft, L.x0, y, L"party", C_TEXT, 0);
    tx = gfx_text(g, ft, tx, y, L"box", C_ACCENT, 0);
    draw_confetti(g, tx + 14 * S, y, ft->line_h, S);
    int title_end = tx + 14 * S + 54 * S;

    /* Firmware info, top right, baseline-aligned with the title. */
    CHAR16 info[160];
    wcopy(info, 160, gST->FirmwareVendor ? gST->FirmwareVendor : L"UEFI");
    uint32_t rev = gST->Hdr.Revision;
    wappend(info, 160, L"  \x00B7  UEFI ");
    wappend_u64(info, 160, rev >> 16);
    wappend(info, 160, L".");
    wappend_u64(info, 160, (rev & 0xffff) / 10);
    if ((rev & 0xffff) % 10) { wappend(info, 160, L"."); wappend_u64(info, 160, (rev & 0xffff) % 10); }
    text_right(g, fs, right, y + ft->ascent - fs->ascent, info, C_FAINT, right - title_end - 24 * S);
    y += ft->line_h;

    CHAR16 sub[300];
    if (u->scan.found) {
        wcopy(sub, 300, u->scan.dir);
        wappend(sub, 300, L"   \x00B7   ");
        int ok = 0;
        for (int i = 0; i < u->scan.count; i++) ok += !u->scan.apps[i].problem;
        wappend_u64(sub, 300, (uint64_t)ok);
        wappend(sub, 300, ok == 1 ? L" app" : L" apps");
        if (u->scan.count > ok) {
            wappend(sub, 300, L"   \x00B7   ");
            wappend_u64(sub, 300, (uint64_t)(u->scan.count - ok));
            wappend(sub, 300, L" can't start");
        }
        if (u->scan.fallback) wappend(sub, 300, L"   \x00B7   found by searching all volumes");
    } else {
        wcopy(sub, 300, L"Create ");
        wappend(sub, 300, u->scan.dir ? u->scan.dir : L"\\PARTYBOX");
        wappend(sub, 300, L" and put some .efi files in it");
    }
    int sub_end = gfx_text(g, fs, L.x0 + 2 * S, y, sub, C_DIM, L.col_w);

    wcopy(info, 160, L"");
    wappend_u64(info, 160, g->w);
    wappend(info, 160, L"\x00D7");
    wappend_u64(info, 160, g->h);
    wappend(info, 160, u->secure_boot ? L"  \x00B7  Secure Boot on" : L"  \x00B7  Secure Boot off");
    wappend(info, 160, L"  \x00B7  v" PARTYBOX_VERSION);
    text_right(g, fs, right, y, info, C_FAINT, right - sub_end - 24 * S);
    y += fs->line_h + 22 * S;

    /* List panel */
    compute_layout(u, &L, y);
    int content;
    int scroll = ensure_visible(u, &L, &content);
    bool scrollbar = content > L.view_h;
    gfx_round_rect(g, L.x0, L.panel_y, L.col_w, L.panel_h, 14 * S, C_PANEL);

    gfx_clip(g, L.view_x, L.view_y, L.view_w, L.view_h);
    int ry = L.view_y - scroll;
    for (int i = 0; i < u->nrows; i++) {
        int h = row_height(&L, &u->rows[i]);
        if (ry + h > L.view_y && ry < L.view_y + L.view_h)
            draw_row(u, &L, &u->rows[i], ry, i == u->sel, scrollbar);
        ry += h;
    }
    gfx_noclip(g);

    if (scrollbar) {
        int track_x = L.view_x + L.view_w - 4 * S, track_h = L.view_h;
        int thumb_h = track_h * L.view_h / content;
        if (thumb_h < 16 * S) thumb_h = 16 * S;
        int thumb_y = L.view_y + (track_h - thumb_h) * scroll / (content - L.view_h);
        gfx_round_rect(g, track_x, L.view_y, 4 * S, track_h, 2 * S, 0x2c2740);
        gfx_round_rect(g, track_x, thumb_y, 4 * S, thumb_h, 2 * S, C_FAINT);
    }

    /* Status line: the autoboot countdown wins over other messages. */
    if (u->countdown) {
        CHAR16 cd[160];
        wcopy(cd, 160, L"Booting ");
        wappend(cd, 160, u->scan.apps[u->rows[u->sel].app].label);
        wappend(cd, 160, L" in ");
        wappend_u64(cd, 160, (uint64_t)u->countdown);
        wappend(cd, 160, L"s  \x00B7  press any key to cancel");
        gfx_text(g, fs, L.x0 + 2 * S, L.stat_y, cd, C_TEXT, L.col_w);
    } else if (u->msg[0]) {
        gfx_text(g, fs, L.x0 + 2 * S, L.stat_y, u->msg, u->msg_color, L.col_w);
    }

    /* Footer */
    const CHAR16 *hints = L"\x2191\x2193  select     \x21B5  launch     R  rescan     Esc  exit";
    gfx_text(g, fs, L.x0 + 2 * S, L.foot_y, hints, C_FAINT, L.col_w);
}

/* Fallback when there is no GOP (e.g. serial-only consoles). */
static void render_text(struct ui *u) {
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *o = gST->ConOut;
    o->SetAttribute(o, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
    o->ClearScreen(o);
    o->OutputString(o, L"partybox " PARTYBOX_VERSION L"  -  ");
    o->OutputString(o, u->scan.found ? u->scan.dir : (CHAR16 *)L"(no PARTYBOX folder found)");
    o->OutputString(o, L"\r\n\r\n");
    for (int i = 0; i < u->nrows; i++) {
        const struct row *r = &u->rows[i];
        bool sel = i == u->sel;
        if (r->kind == ROW_SPACER) { o->OutputString(o, L"\r\n"); continue; }
        o->SetAttribute(o, sel ? EFI_TEXT_ATTR(EFI_BLACK, EFI_LIGHTGRAY)
                               : EFI_TEXT_ATTR(r->kind == ROW_LABEL ? EFI_DARKGRAY : EFI_LIGHTGRAY, EFI_BLACK));
        o->OutputString(o, sel ? L" > " : L"   ");
        if (r->kind == ROW_APP) o->OutputString(o, flag_unsigned(u, &u->scan.apps[r->app]) ? L"! " : L"  ");
        o->OutputString(o, r->kind == ROW_APP ? u->scan.apps[r->app].rel : (CHAR16 *)r->text);
        if (r->kind == ROW_APP && u->scan.apps[r->app].problem) {
            o->OutputString(o, L"  (");
            o->OutputString(o, (CHAR16 *)u->scan.apps[r->app].problem);
            o->OutputString(o, L")");
        }
        o->OutputString(o, L" ");
        o->SetAttribute(o, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
        o->OutputString(o, L"\r\n");
    }
    o->OutputString(o, L"\r\n");
    if (u->countdown) {
        CHAR16 cd[160];
        wcopy(cd, 160, L"Booting ");
        wappend(cd, 160, u->scan.apps[u->rows[u->sel].app].rel);
        wappend(cd, 160, L" in ");
        wappend_u64(cd, 160, (uint64_t)u->countdown);
        wappend(cd, 160, L"s - press any key to cancel\r\n");
        o->OutputString(o, cd);
    } else if (u->msg[0]) {
        o->OutputString(o, u->msg);
        o->OutputString(o, L"\r\n");
    }
    o->OutputString(o, L"Up/Down select, Enter launch, R rescan, Esc exit\r\n");
}

static void draw(struct ui *u) {
    if (u->have_gfx) {
        render(u);
        gfx_present_all(&u->g);
    } else {
        render_text(u);
    }
}

/* ---- actions ---------------------------------------------------------- */

/* Device path of `fs` with a file-path node for `path` appended. */
static EFI_DEVICE_PATH_PROTOCOL *file_device_path(EFI_HANDLE fs, const CHAR16 *path) {
    EFI_DEVICE_PATH_PROTOCOL *dev = NULL;
    size_t dev_len = 0;
    if (!EFI_ERROR(gBS->HandleProtocol(fs, &gEfiDevicePathProtocolGuid, (void **)&dev)) && dev) {
        EFI_DEVICE_PATH_PROTOCOL *n = dev;
        while (!(n->Type == END_DEVICE_PATH_TYPE && n->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE)) {
            size_t len = (size_t)n->Length[0] | ((size_t)n->Length[1] << 8);
            if (len < 4) break;
            dev_len += len;
            n = (EFI_DEVICE_PATH_PROTOCOL *)((uint8_t *)n + len);
        }
    }
    size_t plen = (wlen(path) + 1) * sizeof(CHAR16);
    size_t node_len = 4 + plen;
    uint8_t *buf = xalloc(dev_len + node_len + 4);
    if (!buf) return NULL;
    if (dev_len) memcpy(buf, dev, dev_len);
    EFI_DEVICE_PATH_PROTOCOL *node = (EFI_DEVICE_PATH_PROTOCOL *)(buf + dev_len);
    node->Type = MEDIA_DEVICE_PATH;
    node->SubType = MEDIA_FILEPATH_DP;
    node->Length[0] = (uint8_t)node_len;
    node->Length[1] = (uint8_t)(node_len >> 8);
    memcpy(buf + dev_len + 4, path, plen);
    EFI_DEVICE_PATH_PROTOCOL *end = (EFI_DEVICE_PATH_PROTOCOL *)(buf + dev_len + node_len);
    end->Type = END_DEVICE_PATH_TYPE;
    end->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    end->Length[0] = 4;
    end->Length[1] = 0;
    return (EFI_DEVICE_PATH_PROTOCOL *)buf;
}

static EFI_STATUS read_file(EFI_HANDLE fs, const CHAR16 *path, void **data, UINTN *size) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    EFI_FILE_PROTOCOL *root = NULL, *f = NULL;
    EFI_STATUS st = gBS->HandleProtocol(fs, &gEfiSimpleFileSystemProtocolGuid, (void **)&sfs);
    if (EFI_ERROR(st)) return st;
    if (EFI_ERROR(st = sfs->OpenVolume(sfs, &root))) return st;
    st = root->Open(root, &f, (CHAR16 *)path, EFI_FILE_MODE_READ, 0);
    root->Close(root);
    if (EFI_ERROR(st)) return st;

    UINTN isize = SIZE_OF_EFI_FILE_INFO + 512;
    EFI_FILE_INFO *info = xalloc(isize);
    st = info ? f->GetInfo(f, &gEfiFileInfoGuid, &isize, info) : EFI_OUT_OF_RESOURCES;
    if (!EFI_ERROR(st)) {
        *size = (UINTN)info->FileSize;
        *data = xalloc(*size);
        st = *data ? f->Read(f, size, *data) : EFI_OUT_OF_RESOURCES;
        if (EFI_ERROR(st)) { xfree(*data); *data = NULL; }
    }
    xfree(info);
    f->Close(f);
    return st;
}

static void restore_screen(struct ui *u) {
    gST->ConIn->Reset(gST->ConIn, FALSE); /* drop keys the app left behind */
    gST->ConOut->EnableCursor(gST->ConOut, FALSE);
    if (u->have_gfx) gfx_reclaim(&u->g); /* the app may have changed modes */
}

static void launch(struct ui *u, const struct app *a) {
    CHAR16 *path = scan_app_path(&u->scan, a);
    CHAR16 *name = wdup(a->rel);
    void *img = NULL;
    UINTN size = 0;
    EFI_HANDLE child = NULL;
    CHAR16 err[64] = { 0 };

    EFI_STATUS st = path ? read_file(u->scan.fs_handle, path, &img, &size) : EFI_OUT_OF_RESOURCES;
    if (EFI_ERROR(st)) {
        append_status(err, 64, st);
        set_msg(u, C_ERR, L"Couldn't read ", name, L": ");
        wappend(u->msg, 256, err);
        goto out;
    }

    /* Loading from a buffer with a real device path: the child gets a proper
     * DeviceHandle/FilePath, so it can find its own files (and nested partyboxes work). */
    EFI_DEVICE_PATH_PROTOCOL *dp = file_device_path(u->scan.fs_handle, path);
    st = gBS->LoadImage(FALSE, gImageHandle, dp, img, size, &child);
    xfree(dp);
    xfree(img);
    if (EFI_ERROR(st)) {
        if (child) gBS->UnloadImage(child); /* e.g. Secure Boot rejected it */
        append_status(err, 64, st);
        set_msg(u, C_ERR, L"Couldn't load ", name, L": ");
        wappend(u->msg, 256, err);
        if (u->secure_boot && (st == EFI_ACCESS_DENIED || st == EFI_SECURITY_VIOLATION))
            wappend(u->msg, 256, L" (Secure Boot Violation)");
        goto out;
    }

    gST->ConOut->SetAttribute(gST->ConOut, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
    gST->ConOut->ClearScreen(gST->ConOut);
    gST->ConOut->EnableCursor(gST->ConOut, TRUE);

    UINTN exit_size = 0;
    CHAR16 *exit_data = NULL;
    st = gBS->StartImage(child, &exit_size, &exit_data);

    restore_screen(u);
    append_status(err, 64, st);
    set_msg(u, EFI_ERROR(st) ? C_ERR : C_OK, name, L" exited: ", err);
    if (exit_data) {
        if (exit_size >= sizeof(CHAR16) && exit_data[0]) {
            wappend(u->msg, 256, L"  \x00B7  ");
            wappend(u->msg, 256, exit_data);
        }
        gBS->FreePool(exit_data);
    }
    rescan(u);

out:
    xfree(path);
    xfree(name);
}

static void reboot_to_firmware(struct ui *u) {
    if (!u->fwui_supported) {
        set_msg(u, C_ERR, L"This firmware doesn't support booting into its setup UI", NULL, NULL);
        return;
    }
    uint64_t ind = 0;
    get_u64_var(L"OsIndications", &ind);
    ind |= EFI_OS_INDICATIONS_BOOT_TO_FW_UI;
    EFI_STATUS st = gRT->SetVariable(L"OsIndications", &gEfiGlobalVariableGuid,
                                     EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                                         EFI_VARIABLE_RUNTIME_ACCESS,
                                     sizeof(ind), &ind);
    if (EFI_ERROR(st)) {
        CHAR16 err[64] = { 0 };
        append_status(err, 64, st);
        set_msg(u, C_ERR, L"Couldn't set OsIndications: ", err, NULL);
        return;
    }
    gRT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
}

static EFI_INPUT_KEY wait_key(void) {
    UINTN idx;
    EFI_INPUT_KEY k = { 0, 0 };
    while (EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &k)))
        gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &idx);
    return k;
}

static void draw_button(struct gfx *g, int x, int y, int w, int h, const CHAR16 *text, bool focus) {
    const struct font *f = g->f.body;
    gfx_round_rect(g, x, y, w, h, 8 * g->f.scale, focus ? C_ACCENT : C_BUTTON);
    int tw = gfx_text_width(f, text);
    gfx_text(g, f, x + (w - tw) / 2, y + (h - f->line_h) / 2, text, focus ? C_SEL_TEXT : C_TEXT, 0);
}

/* Modal card over the dimmed menu. focus: 0 = cancel, 1 = confirm. */
static void draw_dialog(struct ui *u, const CHAR16 *title, const CHAR16 *body,
                        const CHAR16 *cancel, const CHAR16 *confirm, int focus) {
    struct gfx *g = &u->g;
    const struct font *fb = g->f.body;
    int S = g->f.scale, W = (int)g->w, H = (int)g->h;
    int dw = W - 48 * S < 540 * S ? W - 48 * S : 540 * S;
    int pad = 28 * S, inner = dw - 2 * pad;
    int chip = fb->line_h;
    int body_h = gfx_text_wrap(NULL, fb, 0, 0, inner, body, 0);
    int btn_h = fb->line_h + 16 * S;
    int dh = pad + chip + 16 * S + body_h + 28 * S + btn_h + pad;
    int dx = (W - dw) / 2, dy = (H - dh) / 2;

    gfx_dim(g, 0, 0, W, H, 0x000000, 150);
    gfx_round_rect(g, dx, dy, dw, dh, 16 * S, C_DIALOG);

    int y = dy + pad;
    draw_warn_chip(g, fb, dx + pad, y, chip);
    gfx_text(g, fb, dx + pad + chip + 12 * S, y + (chip - fb->line_h) / 2, title, C_TEXT, inner - chip - 12 * S);
    y += chip + 16 * S;
    gfx_text_wrap(g, fb, dx + pad, y, inner, body, 0xcdc7df);
    y += body_h + 28 * S;

    int bw1 = gfx_text_width(fb, cancel) + 40 * S, bw2 = gfx_text_width(fb, confirm) + 40 * S;
    int bx2 = dx + dw - pad - bw2, bx1 = bx2 - 12 * S - bw1;
    draw_button(g, bx1, y, bw1, btn_h, cancel, focus == 0);
    draw_button(g, bx2, y, bw2, btn_h, confirm, focus == 1);
}

/* Secure Boot is on and the image carries no signature: ask first. */
static bool confirm_unsigned(struct ui *u, const struct app *a) {
    CHAR16 body[300];
    wcopy(body, 300, L"Secure Boot is enabled, but ");
    wappend(body, 300, a->rel);
    wappend(body, 300, L" is not signed. This might cause failure to boot. Do you want to continue anyway?");

    if (!u->have_gfx) {
        EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *o = gST->ConOut;
        o->OutputString(o, L"\r\n! ");
        o->OutputString(o, body);
        o->OutputString(o, L" [y/N] ");
        EFI_INPUT_KEY k = wait_key();
        return k.UnicodeChar == 'y' || k.UnicodeChar == 'Y';
    }

    int focus = 0; /* default to the safe choice */
    for (;;) {
        render(u);
        draw_dialog(u, L"Unsigned image", body, L"Cancel", L"Continue anyway", focus);
        gfx_present_all(&u->g);
        EFI_INPUT_KEY k = wait_key();
        if (k.ScanCode == SCAN_LEFT || k.ScanCode == SCAN_RIGHT || k.UnicodeChar == CHAR_TAB) focus ^= 1;
        else if (k.ScanCode == SCAN_ESC || k.UnicodeChar == 'n' || k.UnicodeChar == 'N') return false;
        else if (k.UnicodeChar == 'y' || k.UnicodeChar == 'Y') return true;
        else if (k.UnicodeChar == CHAR_CARRIAGE_RETURN || k.UnicodeChar == ' ') return focus == 1;
    }
}

/* Returns false when partybox should exit. */
static bool activate(struct ui *u) {
    const struct row *r = &u->rows[u->sel];
    switch (r->kind) {
    case ROW_APP: {
        const struct app *a = &u->scan.apps[r->app];
        if (a->problem) {
            set_msg(u, C_ERR, a->rel, L" can't be started here: ", a->problem);
            return true;
        }
        if (flag_unsigned(u, a) && !confirm_unsigned(u, a)) return true;
        launch(u, a);
        return true;
    }
    case ROW_REBOOT: gRT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL); return true;
    case ROW_SHUTDOWN: gRT->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL); return true;
    case ROW_FWUI: reboot_to_firmware(u); return true;
    case ROW_EXIT: return false;
    default: return true;
    }
}

/* Returns false when partybox should exit. */
static bool handle_key(struct ui *u, EFI_INPUT_KEY k) {
    int page = 8;
    switch (k.ScanCode) {
    case SCAN_UP: move_sel(u, -1); return true;
    case SCAN_DOWN: move_sel(u, 1); return true;
    case SCAN_PAGE_UP: move_sel(u, -page); return true;
    case SCAN_PAGE_DOWN: move_sel(u, page); return true;
    case SCAN_HOME: move_sel(u, -u->nrows); return true;
    case SCAN_END: move_sel(u, u->nrows); return true;
    case SCAN_F5: rescan(u); u->msg[0] = 0; return true;
    case SCAN_ESC: return false;
    default: break;
    }
    switch (k.UnicodeChar) {
    case CHAR_CARRIAGE_RETURN: case ' ': return activate(u);
    case 'k': move_sel(u, -1); return true;
    case 'j': move_sel(u, 1); return true;
    case 'r': case 'R':
        rescan(u);
        set_msg(u, C_DIM, L"Rescanned", NULL, NULL);
        return true;
    default: return true;
    }
}

/* Look for "timeout=N" (seconds, 0 = off) in our load options, e.g. Limine's
 * `cmdline: timeout=10` or arguments from the UEFI shell. */
static int parse_timeout(void) {
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    if (EFI_ERROR(gBS->HandleProtocol(gImageHandle, &gEfiLoadedImageProtocolGuid, (void **)&li)) || !li ||
        !li->LoadOptions || li->LoadOptionsSize < 2 * 8)
        return DEFAULT_TIMEOUT;
    const CHAR16 *s = li->LoadOptions;
    size_t n = li->LoadOptionsSize / sizeof(CHAR16);
    static const CHAR16 key[] = L"timeout=";
    for (size_t i = 0; i + 8 <= n; i++) {
        if (i && s[i - 1] != ' ') continue; /* token start only */
        size_t j = 0;
        while (j < 8 && wlower(s[i + j]) == key[j]) j++;
        if (j < 8) continue;
        int v = 0;
        bool any = false;
        for (size_t k = i + 8; k < n && s[k] >= '0' && s[k] <= '9' && v < 100000; k++, any = true)
            v = v * 10 + (s[k] - '0');
        if (any) return v;
    }
    return DEFAULT_TIMEOUT;
}

static void start_countdown(struct ui *u, int seconds) {
    if (seconds <= 0 || !launchable_row(u, u->sel)) return;
    if (EFI_ERROR(gBS->CreateEvent(EVT_TIMER, 0, NULL, NULL, &u->timer))) return;
    if (EFI_ERROR(gBS->SetTimer(u->timer, TimerPeriodic, 10000000 /* 1 s in 100 ns units */))) {
        gBS->CloseEvent(u->timer);
        return;
    }
    u->countdown = seconds;
}

static void stop_countdown(struct ui *u) {
    if (u->timer) {
        gBS->SetTimer(u->timer, TimerCancel, 0);
        gBS->CloseEvent(u->timer);
        u->timer = NULL;
    }
    u->countdown = 0;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    gImageHandle = image;
    gST = st;
    gBS = st->BootServices;
    gRT = st->RuntimeServices;

    gBS->SetWatchdogTimer(0, 0, 0, NULL); /* a menu may sit for a while */

    static struct ui u;
    memset(&u, 0, sizeof(u));
    u.have_gfx = !EFI_ERROR(gfx_init(&u.g));
    gST->ConOut->EnableCursor(gST->ConOut, FALSE);
    if (!u.have_gfx) gST->ConOut->ClearScreen(gST->ConOut);

    probe_firmware(&u);
    rescan(&u);
    start_countdown(&u, parse_timeout());
    draw(&u);

    for (;;) {
        UINTN idx = 0;
        EFI_INPUT_KEY k;
        EFI_EVENT events[2] = { gST->ConIn->WaitForKey, u.timer };
        gBS->WaitForEvent(u.countdown ? 2 : 1, events, &idx);
        if (u.countdown && idx == 1) {
            bool run = true;
            if (--u.countdown == 0) {
                stop_countdown(&u);
                run = activate(&u);
            }
            if (!run) break;
            draw(&u);
            continue;
        }
        stop_countdown(&u); /* any key cancels autoboot */
        bool run = true;
        /* Drain everything queued (key repeat) before redrawing once. */
        while (run && !EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &k))) run = handle_key(&u, k);
        if (!run) break;
        draw(&u);
    }

    gST->ConOut->SetAttribute(gST->ConOut, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
    gST->ConOut->ClearScreen(gST->ConOut);
    gST->ConOut->EnableCursor(gST->ConOut, TRUE);
    scan_free(&u.scan);
    xfree(u.rows);
    xfree(u.g.back);
    return EFI_SUCCESS;
}
