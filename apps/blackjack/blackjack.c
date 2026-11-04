/* blackjack.efi -- a small GOP blackjack table, built to run from partybox.
 *
 * Deliberately conservative about firmware: GOP Blt for output, ConIn for
 * input, Stall for pacing. No floats, no timers, no other protocols.
 *
 * Rules: 6-deck shoe, dealer stands on all 17s and peeks for blackjack,
 * blackjack pays 3:2, double on the first two cards. No split/insurance.
 */
#include "lib.h"
#include "gfx.h"
#include "font_cards.h"

EFI_HANDLE gImageHandle;
EFI_SYSTEM_TABLE *gST;
EFI_BOOT_SERVICES *gBS;
EFI_RUNTIME_SERVICES *gRT;

#define DECKS       6
#define SHOE_N      (52 * DECKS)
#define START_CHIPS 1000
#define MIN_BET     10
#define MAX_BET     500
#define DEAL_US     260000 /* pause between dealt cards */

/* ---- palette ---------------------------------------------------------- */
#define C_FELT_TOP  0x14613b
#define C_FELT_BOT  0x0a3a23
#define C_SHADOW    0x07291a
#define C_TABLE_TXT 0xcdb56c
#define C_LABEL     0xd6eedd
#define C_DIM       0x8fbfa3
#define C_CARD      0xfbfaf5
#define C_CARD_EDGE 0xc9c6bb
#define C_RED       0xc8102e
#define C_BLACK     0x1c1c1c
#define C_BACK      0xb3243c
#define C_BACK_DOT  0xd4475e
#define C_BANNER    0x08301d
#define C_HUD       0x061f13
#define C_WIN       0x9be7a8
#define C_GOLD      0xffd166
#define C_LOSS      0xff8a80
#define C_PUSH      0xe6eee8
#define C_CHIP      0xc62828
#define C_WHITE     0xffffff

enum phase { BETTING, PLAYER, OVER };

struct hand {
    uint8_t c[16]; /* card = (rank - 1) * 4 + suit, rank 1..13 */
    int n;
};

struct game {
    struct gfx g;
    const struct font *rank, *pip, *suit;
    int S, cw, ch;

    uint8_t shoe[SHOE_N];
    int pos;
    uint64_t rng;

    struct hand dealer, player;
    bool hole_hidden;
    int chips, bet, stake;
    enum phase phase;

    CHAR16 msg[96], sub[96];
    color_t msg_color;
};

/* ---- randomness --------------------------------------------------------- */
static uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

static void rng_mix(struct game *G, uint64_t v) {
    G->rng ^= v + 0x9e3779b97f4a7c15ULL + (G->rng << 6) + (G->rng >> 2);
    if (!G->rng) G->rng = 0x853c49e6748fea9bULL;
}

static uint64_t rng_next(struct game *G) { /* xorshift64* */
    uint64_t x = G->rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    G->rng = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static uint32_t rng_below(struct game *G, uint32_t n) { /* unbiased */
    uint64_t limit = UINT64_MAX - UINT64_MAX % n, r;
    do r = rng_next(G); while (r >= limit);
    return (uint32_t)(r % n);
}

static void seed(struct game *G) {
    EFI_TIME t;
    G->rng = 0;
    rng_mix(G, rdtsc());
    if (!EFI_ERROR(gRT->GetTime(&t, NULL)))
        rng_mix(G, (uint64_t)t.Year << 40 | (uint64_t)t.Month << 32 | (uint64_t)t.Day << 24 |
                       (uint64_t)t.Hour << 16 | (uint64_t)t.Minute << 8 | t.Second);
    rng_mix(G, rdtsc());
}

/* ---- cards --------------------------------------------------------------- */
static int card_rank(uint8_t c) { return c / 4 + 1; }
static int card_suit(uint8_t c) { return c % 4; }

static void shuffle(struct game *G) {
    for (int i = 0; i < SHOE_N; i++) G->shoe[i] = (uint8_t)(i % 52);
    for (int i = SHOE_N - 1; i > 0; i--) {
        int j = (int)rng_below(G, (uint32_t)i + 1);
        uint8_t t = G->shoe[i];
        G->shoe[i] = G->shoe[j];
        G->shoe[j] = t;
    }
    G->pos = 0;
}

/* Best total of the first `upto` cards; *soft if an ace counts as 11. */
static int hand_value(const struct hand *h, int upto, bool *soft) {
    int sum = 0;
    bool ace = false;
    for (int i = 0; i < upto && i < h->n; i++) {
        int r = card_rank(h->c[i]);
        sum += r > 10 ? 10 : r;
        ace |= r == 1;
    }
    bool s = ace && sum + 10 <= 21;
    if (soft) *soft = s;
    return s ? sum + 10 : sum;
}

static int value(const struct hand *h) { return hand_value(h, h->n, NULL); }
static bool blackjack(const struct hand *h) { return h->n == 2 && value(h) == 21; }

/* ---- drawing --------------------------------------------------------------- */
static const CHAR16 *rank_str(int r) {
    static const CHAR16 *names[] = { L"A", L"2", L"3", L"4", L"5", L"6", L"7",
                                     L"8", L"9", L"10", L"J", L"Q", L"K" };
    return names[r - 1];
}

static const CHAR16 suit_cp[4] = { 0x2660, 0x2665, 0x2666, 0x2663 };

/* Draw s so that its first glyph's ink is vertically centred on cy and the
 * whole string is horizontally centred on cx. */
static void text_center(struct gfx *g, const struct font *f, int cx, int cy, const CHAR16 *s, color_t c) {
    const struct glyph *ref = gfx_glyph(f, s[0]);
    gfx_text(g, f, cx - gfx_text_width(f, s) / 2, cy - (ref->by + ref->h / 2), s, c, 0);
}

static void draw_card(struct game *G, int x, int y, uint8_t card, bool face_down) {
    struct gfx *g = &G->g;
    int w = G->cw, h = G->ch, S = G->S, r = w / 11;

    gfx_round_rect(g, x + 3 * S, y + 5 * S, w, h, r, C_SHADOW);
    gfx_round_rect(g, x - 1, y - 1, w + 2, h + 2, r + 1, C_CARD_EDGE);
    gfx_round_rect(g, x, y, w, h, r, C_CARD);

    if (face_down) {
        int in = w / 12, step = 11 * S;
        gfx_round_rect(g, x + in, y + in, w - 2 * in, h - 2 * in, r * 2 / 3, C_BACK);
        gfx_clip(g, x + in + 2 * S, y + in + 2 * S, w - 2 * in - 4 * S, h - 2 * in - 4 * S);
        for (int row = 0, yy = y + in; yy < y + h - in; yy += step / 2, row++)
            for (int xx = x + in + (row & 1) * step / 2; xx < x + w - in; xx += step)
                gfx_round_rect(g, xx - 2 * S, yy - 2 * S, 4 * S, 4 * S, 2 * S, C_BACK_DOT);
        gfx_noclip(g);
        return;
    }

    int rk = card_rank(card), st = card_suit(card);
    color_t col = (st == 1 || st == 2) ? C_RED : C_BLACK;
    CHAR16 suit[2] = { suit_cp[st], 0 };

    /* corner index: rank with a small suit under it */
    const CHAR16 *rs = rank_str(rk);
    int ix = x + w / 14 + gfx_text_width(G->rank, L"10") / 2;
    text_center(g, G->rank, ix, y + h * 11 / 100, rs, col);
    text_center(g, G->pip, ix, y + h * 24 / 100, suit, col);

    /* centre: big suit; court cards get a frame like a printed face */
    if (rk > 10) {
        int fx = x + w * 26 / 100, fy = y + h * 20 / 100, fw = w - (fx - x) - w * 12 / 100,
            fh = h - (fy - y) - h * 10 / 100;
        gfx_round_rect(g, fx, fy, fw, fh, 4 * S, col);
        gfx_round_rect(g, fx + 2 * S, fy + 2 * S, fw - 4 * S, fh - 4 * S, 3 * S, C_CARD);
        text_center(g, G->rank, fx + fw / 2, fy + fh * 30 / 100, rs, col);
        text_center(g, G->suit, fx + fw / 2, fy + fh * 64 / 100, suit, col);
    } else {
        text_center(g, G->suit, x + w * 56 / 100, y + h * 56 / 100, suit, col);
    }
}

/* Lay a hand out centred at (cx, y), overlapping cards to fit max_w. */
static void draw_hand(struct game *G, const struct hand *h, int cx, int y, int max_w, bool hide_second) {
    if (!h->n) return;
    int step = G->cw * 62 / 100;
    if (h->n > 1 && G->cw + (h->n - 1) * step > max_w) step = (max_w - G->cw) / (h->n - 1);
    int x = cx - (G->cw + (h->n - 1) * step) / 2;
    for (int i = 0; i < h->n; i++) draw_card(G, x + i * step, y, h->c[i], hide_second && i == 1);
}

static void fmt_chips(CHAR16 *dst, size_t cap, int v) {
    CHAR16 digits[16];
    int n = 0;
    do { digits[n++] = (CHAR16)('0' + v % 10); v /= 10; } while (v && n < 15);
    size_t o = wlen(dst);
    for (int i = n - 1; i >= 0 && o + 1 < cap; i--) {
        dst[o++] = digits[i];
        if (i && i % 3 == 0 && o + 1 < cap) dst[o++] = ',';
    }
    dst[o] = 0;
}

/* "DEALER" + a total pill, centred on cx, text vertically centred on cy. */
static void draw_label(struct game *G, int cx, int cy, const CHAR16 *who, const CHAR16 *total) {
    struct gfx *g = &G->g;
    const struct font *fs = g->f.small, *fb = g->f.body;
    int S = G->S, gap = 10 * S, pad = 10 * S;
    int ww = gfx_text_width(fs, who), tw = total ? gfx_text_width(fb, total) + 2 * pad : 0;
    int x = cx - (ww + (total ? gap + tw : 0)) / 2;
    gfx_text(g, fs, x, cy - fs->line_h / 2, who, C_LABEL, 0);
    if (total) {
        int ph = fb->line_h + 4 * S, px = x + ww + gap;
        gfx_round_rect(g, px, cy - ph / 2, tw, ph, ph / 2, C_BANNER);
        gfx_text(g, fb, px + pad, cy - fb->line_h / 2, total, C_WHITE, 0);
    }
}

static void total_str(CHAR16 *buf, size_t cap, const struct hand *h, int upto) {
    bool soft;
    int v = hand_value(h, upto, &soft);
    buf[0] = 0;
    if (soft && v < 21) { /* "7 / 17" reads better than "soft 17" at a glance */
        wappend_u64(buf, cap, (uint64_t)(v - 10));
        wappend(buf, cap, L" / ");
    }
    wappend_u64(buf, cap, (uint64_t)v);
}

static void draw_chip(struct game *G, int cx, int cy, int amount) {
    struct gfx *g = &G->g;
    int S = G->S, R = 26 * S;
    gfx_round_rect(g, cx - R + 2 * S, cy - R + 3 * S, 2 * R, 2 * R, R, C_SHADOW);
    gfx_round_rect(g, cx - R, cy - R, 2 * R, 2 * R, R, C_CHIP);
    int r2 = R - 5 * S;
    gfx_round_rect(g, cx - r2, cy - r2, 2 * r2, 2 * r2, r2, C_WHITE);
    int r3 = R - 8 * S;
    gfx_round_rect(g, cx - r3, cy - r3, 2 * r3, 2 * r3, r3, C_CHIP);
    CHAR16 s[16] = { 0 };
    fmt_chips(s, 16, amount);
    const struct font *f = gfx_text_width(g->f.body, s) < 2 * r3 - 6 * S ? g->f.body : g->f.small;
    text_center(g, f, cx, cy, s, C_WHITE);
}

static void render(struct game *G) {
    struct gfx *g = &G->g;
    int W = (int)g->w, H = (int)g->h, S = G->S;
    const struct font *fs = g->f.small, *fb = g->f.body;
    CHAR16 buf[96];

    gfx_noclip(g);
    gfx_vgradient(g, 0, 0, W, H, C_FELT_TOP, C_FELT_BOT);

    int hud_h = 64 * S, max_w = W - 64 * S;
    int label_h = fb->line_h + 4 * S;
    int dealer_label_y = H * 6 / 100 + label_h / 2;
    int dealer_y = dealer_label_y + label_h / 2 + 14 * S;
    int mid_y = dealer_y + G->ch + (H - hud_h - 2 * G->ch - dealer_y - 2 * label_h) / 2 + 6 * S;
    int player_y = H - hud_h - 26 * S - label_h - G->ch;
    if (player_y < mid_y + 30 * S) player_y = mid_y + 30 * S;
    int player_label_y = player_y + G->ch + 14 * S + label_h / 2;

    /* dealer */
    if (G->dealer.n) {
        if (G->hole_hidden) {
            total_str(buf, 96, &G->dealer, 1);
            wappend(buf, 96, L" + ?");
        } else {
            total_str(buf, 96, &G->dealer, G->dealer.n);
        }
        draw_label(G, W / 2, dealer_label_y, L"DEALER", buf);
    } else {
        draw_label(G, W / 2, dealer_label_y, L"DEALER", NULL);
    }
    draw_hand(G, &G->dealer, W / 2, dealer_y, max_w, G->hole_hidden);

    /* middle: result banner, or the table's printed rules */
    if (G->msg[0]) {
        int tw = gfx_text_width(fb, G->msg), sw = G->sub[0] ? gfx_text_width(fs, G->sub) : 0;
        int bw = (tw > sw ? tw : sw) + 56 * S, bh = fb->line_h + (G->sub[0] ? fs->line_h : 0) + 24 * S;
        gfx_round_rect(g, W / 2 - bw / 2, mid_y - bh / 2, bw, bh, 14 * S, C_BANNER);
        int ty = mid_y - bh / 2 + 12 * S;
        gfx_text(g, fb, W / 2 - tw / 2, ty, G->msg, G->msg_color, 0);
        if (G->sub[0]) gfx_text(g, fs, W / 2 - sw / 2, ty + fb->line_h, G->sub, C_DIM, 0);
    } else {
        const CHAR16 *rules = L"BLACKJACK PAYS 3 TO 2   \x00B7   DEALER STANDS ON ALL 17s";
        gfx_text(g, fs, W / 2 - gfx_text_width(fs, rules) / 2, mid_y - fs->line_h / 2, rules, C_TABLE_TXT, 0);
    }

    /* player */
    draw_hand(G, &G->player, W / 2, player_y, max_w, false);
    if (G->player.n) {
        total_str(buf, 96, &G->player, G->player.n);
        draw_label(G, W / 2, player_label_y, L"YOU", buf);
    }

    /* HUD */
    int hy = H - hud_h;
    gfx_fill(g, 0, hy, W, hud_h, C_HUD);
    int cy = hy + hud_h / 2, lx = 32 * S;
    gfx_text(g, fs, lx, cy - fs->line_h, L"CHIPS", C_DIM, 0);
    buf[0] = 0;
    fmt_chips(buf, 96, G->chips);
    gfx_text(g, fb, lx, cy - 2 * S, buf, C_WHITE, 0);

    int amount = G->phase == BETTING ? G->bet : G->stake;
    if (amount) draw_chip(G, W / 2, cy, amount);

    const CHAR16 *hints;
    switch (G->phase) {
    case BETTING: hints = L"\x2191\x2193  bet     \x21B5  deal     Esc  quit"; break;
    case PLAYER:
        hints = G->player.n == 2 && G->chips >= G->stake ? L"H  hit     S  stand     D  double     Esc  quit"
                                                          : L"H  hit     S  stand     Esc  quit";
        break;
    default: hints = L"\x21B5  next hand     Esc  quit"; break;
    }
    int hw = gfx_text_width(fs, hints);
    gfx_text(g, fs, W - 32 * S - hw, cy - fs->line_h / 2, hints, C_DIM, 0);
    if (G->phase == BETTING) {
        const CHAR16 *t = L"PLACE YOUR BET";
        gfx_text(g, fs, W / 2 + 42 * S, cy - fs->line_h / 2, t, C_TABLE_TXT, 0);
    }

    gfx_present_all(g);
}

/* ---- game flow ----------------------------------------------------------- */
static void pause_us(UINTN us) { gBS->Stall(us); }

static void deal(struct game *G, struct hand *h) {
    if (G->pos >= SHOE_N) shuffle(G); /* can't happen with the cut card, but be safe */
    if (h->n < (int)sizeof(h->c)) h->c[h->n++] = G->shoe[G->pos++];
    render(G);
    pause_us(DEAL_US);
}

static void set_msg(struct game *G, color_t c, const CHAR16 *a, int amount, const CHAR16 *sub) {
    G->msg_color = c;
    wcopy(G->msg, 96, a);
    if (amount > 0) { wappend(G->msg, 96, L"   +"); fmt_chips(G->msg, 96, amount); }
    if (amount < 0) { wappend(G->msg, 96, L"   -"); fmt_chips(G->msg, 96, -amount); }
    wcopy(G->sub, 96, sub ? sub : L"");
}

static void settle(struct game *G) {
    G->hole_hidden = false;
    int pv = value(&G->player), dv = value(&G->dealer), s = G->stake;
    bool pbj = blackjack(&G->player), dbj = blackjack(&G->dealer);

    if (pbj && dbj) { G->chips += s; set_msg(G, C_PUSH, L"Push \x00B7 both have blackjack", 0, NULL); }
    else if (pbj) { G->chips += s + s * 3 / 2; set_msg(G, C_GOLD, L"Blackjack!", s * 3 / 2, NULL); }
    else if (dbj) set_msg(G, C_LOSS, L"Dealer has blackjack", -s, NULL);
    else if (pv > 21) set_msg(G, C_LOSS, L"Bust", -s, NULL);
    else if (dv > 21) { G->chips += 2 * s; set_msg(G, C_WIN, L"Dealer busts", s, NULL); }
    else if (pv > dv) { G->chips += 2 * s; set_msg(G, C_WIN, L"You win", s, NULL); }
    else if (pv == dv) { G->chips += s; set_msg(G, C_PUSH, L"Push", 0, NULL); }
    else set_msg(G, C_LOSS, L"Dealer wins", -s, NULL);

    if (G->chips < MIN_BET) wcopy(G->sub, 96, L"Out of chips \x00B7 press Enter for a fresh stack");
    G->phase = OVER;
}

static void dealer_play(struct game *G) {
    G->hole_hidden = false;
    render(G);
    pause_us(DEAL_US);
    while (value(&G->dealer) < 17) deal(G, &G->dealer); /* stands on all 17s */
    settle(G);
}

static void start_round(struct game *G) {
    G->msg[0] = G->sub[0] = 0;
    if (G->pos > SHOE_N * 3 / 4) {
        set_msg(G, C_PUSH, L"Shuffling\x2026", 0, NULL);
        G->dealer.n = G->player.n = 0;
        render(G);
        shuffle(G);
        pause_us(700000);
        G->msg[0] = 0;
    }
    G->stake = G->bet;
    G->chips -= G->stake;
    G->dealer.n = G->player.n = 0;
    G->hole_hidden = true;
    G->phase = PLAYER;

    deal(G, &G->player);
    deal(G, &G->dealer);
    deal(G, &G->player);
    deal(G, &G->dealer);

    /* Dealer peeks under an Ace or ten; naturals settle immediately. */
    int up = card_rank(G->dealer.c[0]);
    bool peek = up == 1 || up >= 10;
    if (blackjack(&G->player) || (peek && blackjack(&G->dealer))) settle(G);
}

static void clamp_bet(struct game *G) {
    int max = G->chips < MAX_BET ? G->chips : MAX_BET;
    if (G->bet > max) G->bet = max - max % MIN_BET;
    if (G->bet < MIN_BET) G->bet = MIN_BET;
}

static void on_key(struct game *G, EFI_INPUT_KEY k) {
    CHAR16 ch = wlower(k.UnicodeChar);
    switch (G->phase) {
    case BETTING:
        if (k.ScanCode == SCAN_UP || k.ScanCode == SCAN_RIGHT) G->bet += MIN_BET;
        else if (k.ScanCode == SCAN_DOWN || k.ScanCode == SCAN_LEFT) G->bet -= MIN_BET;
        else if (k.ScanCode == SCAN_PAGE_UP) G->bet += 100;
        else if (k.ScanCode == SCAN_PAGE_DOWN) G->bet -= 100;
        else if (ch == CHAR_CARRIAGE_RETURN || ch == ' ') { start_round(G); return; }
        clamp_bet(G);
        break;
    case PLAYER:
        if (ch == 'h') {
            deal(G, &G->player);
            int v = value(&G->player);
            if (v > 21) settle(G);
            else if (v == 21) dealer_play(G);
        } else if (ch == 's') {
            dealer_play(G);
        } else if (ch == 'd' && G->player.n == 2 && G->chips >= G->stake) {
            G->chips -= G->stake;
            G->stake *= 2;
            deal(G, &G->player);
            if (value(&G->player) > 21) settle(G);
            else dealer_play(G);
        }
        break;
    case OVER:
        if (ch == CHAR_CARRIAGE_RETURN || ch == ' ') {
            G->msg[0] = G->sub[0] = 0;
            G->dealer.n = G->player.n = 0;
            G->stake = 0;
            if (G->chips < MIN_BET) {
                G->chips = START_CHIPS;
                set_msg(G, C_PUSH, L"Fresh stack", START_CHIPS, NULL);
            }
            clamp_bet(G);
            G->phase = BETTING;
        }
        break;
    }
}

static EFI_INPUT_KEY wait_key(void) {
    UINTN idx;
    EFI_INPUT_KEY k = { 0, 0 };
    while (EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &k)))
        gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &idx);
    return k;
}

static void pick_layout(struct game *G) {
    struct gfx *g = &G->g;
    G->S = g->f.scale;
    if (G->S == 2) { G->rank = &font_rank_2x; G->pip = &font_pip_2x; G->suit = &font_suit_2x; }
    else { G->rank = &font_rank_1x; G->pip = &font_pip_1x; G->suit = &font_suit_1x; }
    /* Two rows of cards plus labels, banner and HUD must fit vertically. */
    int lo = 120 * G->S, hi = 230 * G->S;
    G->ch = (int)g->h * 23 / 100;
    if (G->ch < lo) G->ch = lo;
    if (G->ch > hi) G->ch = hi;
    G->cw = G->ch * 5 / 7;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    gImageHandle = image;
    gST = st;
    gBS = st->BootServices;
    gRT = st->RuntimeServices;
    gBS->SetWatchdogTimer(0, 0, 0, NULL);

    static struct game G;
    memset(&G, 0, sizeof(G));
    if (EFI_ERROR(gfx_init(&G.g))) {
        gST->ConOut->OutputString(gST->ConOut, L"blackjack needs a graphics (GOP) display.\r\n");
        gBS->Stall(3000000);
        return EFI_UNSUPPORTED;
    }
    gST->ConOut->EnableCursor(gST->ConOut, FALSE);
    pick_layout(&G);

    seed(&G);
    shuffle(&G);
    G.chips = START_CHIPS;
    G.bet = 50;
    G.phase = BETTING;

    for (;;) {
        render(&G);
        EFI_INPUT_KEY k = wait_key();
        rng_mix(&G, rdtsc()); /* key timing keeps the shoe unpredictable */
        if (k.ScanCode == SCAN_ESC) break;
        on_key(&G, k);
        /* Drop keys pressed while cards were being dealt. */
        if (G.phase != BETTING) gST->ConIn->Reset(gST->ConIn, FALSE);
    }

    xfree(G.g.back);
    return EFI_SUCCESS;
}
