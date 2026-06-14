/*
 * V3 Dashboard — landscape layout (logical 296×128 px).
 *
 * Physical buffer: 128 wide × 296 tall (portrait, SSD1680).
 * Coordinate rotation 90° CW: logical (lx, ly) → physical (ly, 295−lx).
 *
 * Color convention in the physical buffer:
 *   bw_buf  bit 1 = white,  bit 0 = black
 *   red_buf bit 1 = red,    bit 0 = no-red
 *
 * After memset(bw_buf, 0xFF) + memset(red_buf, 0x00) the canvas is all-white.
 * Drawing "black" clears a bw bit; drawing "red" sets a red bit (bw stays 1).
 */

#include "display.h"
#include "eink_weact29.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "qrcode.h"
#include "sdkconfig.h"
#include "wifi_prov.h"
#include "runtime_policy.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "display";

typedef struct {
    bool valid;
    uint8_t wifi_slot;
    uint8_t active_api_slot;
} header_connection_slots_t;

static header_connection_slots_t s_header_slots;

/* ── 5×7 pixel font (ASCII 32–122) ─────────────────────────────────────── */
#define FONT_W  6   /* 5 px glyph + 1 px gap */
#define FONT_H  7
#define FONT_BOOT_W 6
#define FONT2_W 12  /* 2× scaled: 10 px + 2 px gap */
#define FONT2_H 14

static const uint8_t font5x7[][5] = {
    /* Space */ {0x00,0x00,0x00,0x00,0x00},
    /* !     */ {0x00,0x00,0x5F,0x00,0x00},
    /* "     */ {0x00,0x07,0x00,0x07,0x00},
    /* #     */ {0x14,0x7F,0x14,0x7F,0x14},
    /* $     */ {0x24,0x2A,0x7F,0x2A,0x12},
    /* %     */ {0x23,0x13,0x08,0x64,0x62},
    /* &     */ {0x36,0x49,0x55,0x22,0x50},
    /* '     */ {0x00,0x05,0x03,0x00,0x00},
    /* (     */ {0x00,0x1C,0x22,0x41,0x00},
    /* )     */ {0x00,0x41,0x22,0x1C,0x00},
    /* *     */ {0x14,0x08,0x3E,0x08,0x14},
    /* +     */ {0x08,0x08,0x3E,0x08,0x08},
    /* ,     */ {0x00,0x50,0x30,0x00,0x00},
    /* -     */ {0x08,0x08,0x08,0x08,0x08},
    /* .     */ {0x00,0x60,0x60,0x00,0x00},
    /* /     */ {0x20,0x10,0x08,0x04,0x02},
    /* 0     */ {0x3E,0x51,0x49,0x45,0x3E},
    /* 1     */ {0x00,0x42,0x7F,0x40,0x00},
    /* 2     */ {0x42,0x61,0x51,0x49,0x46},
    /* 3     */ {0x21,0x41,0x45,0x4B,0x31},
    /* 4     */ {0x18,0x14,0x12,0x7F,0x10},
    /* 5     */ {0x27,0x45,0x45,0x45,0x39},
    /* 6     */ {0x3C,0x4A,0x49,0x49,0x30},
    /* 7     */ {0x01,0x71,0x09,0x05,0x03},
    /* 8     */ {0x36,0x49,0x49,0x49,0x36},
    /* 9     */ {0x06,0x49,0x49,0x29,0x1E},
    /* :     */ {0x00,0x36,0x36,0x00,0x00},
    /* ;     */ {0x00,0x56,0x36,0x00,0x00},
    /* <     */ {0x08,0x14,0x22,0x41,0x00},
    /* =     */ {0x14,0x14,0x14,0x14,0x14},
    /* >     */ {0x00,0x41,0x22,0x14,0x08},
    /* ?     */ {0x02,0x01,0x51,0x09,0x06},
    /* @     */ {0x32,0x49,0x79,0x41,0x3E},
    /* A     */ {0x7E,0x11,0x11,0x11,0x7E},
    /* B     */ {0x7F,0x49,0x49,0x49,0x36},
    /* C     */ {0x3E,0x41,0x41,0x41,0x22},
    /* D     */ {0x7F,0x41,0x41,0x22,0x1C},
    /* E     */ {0x7F,0x49,0x49,0x49,0x41},
    /* F     */ {0x7F,0x09,0x09,0x09,0x01},
    /* G     */ {0x3E,0x41,0x49,0x49,0x7A},
    /* H     */ {0x7F,0x08,0x08,0x08,0x7F},
    /* I     */ {0x00,0x41,0x7F,0x41,0x00},
    /* J     */ {0x20,0x40,0x41,0x3F,0x01},
    /* K     */ {0x7F,0x08,0x14,0x22,0x41},
    /* L     */ {0x7F,0x40,0x40,0x40,0x40},
    /* M     */ {0x7F,0x02,0x0C,0x02,0x7F},
    /* N     */ {0x7F,0x04,0x08,0x10,0x7F},
    /* O     */ {0x3E,0x41,0x41,0x41,0x3E},
    /* P     */ {0x7F,0x09,0x09,0x09,0x06},
    /* Q     */ {0x3E,0x41,0x51,0x21,0x5E},
    /* R     */ {0x7F,0x09,0x19,0x29,0x46},
    /* S     */ {0x46,0x49,0x49,0x49,0x31},
    /* T     */ {0x01,0x01,0x7F,0x01,0x01},
    /* U     */ {0x3F,0x40,0x40,0x40,0x3F},
    /* V     */ {0x1F,0x20,0x40,0x20,0x1F},
    /* W     */ {0x3F,0x40,0x38,0x40,0x3F},
    /* X     */ {0x63,0x14,0x08,0x14,0x63},
    /* Y     */ {0x07,0x08,0x70,0x08,0x07},
    /* Z     */ {0x61,0x51,0x49,0x45,0x43},
    /* [     */ {0x00,0x7F,0x41,0x41,0x00},
    /* \     */ {0x02,0x04,0x08,0x10,0x20},
    /* ]     */ {0x00,0x41,0x41,0x7F,0x00},
    /* ^     */ {0x04,0x02,0x01,0x02,0x04},
    /* _     */ {0x40,0x40,0x40,0x40,0x40},
    /* `     */ {0x00,0x01,0x02,0x04,0x00},
    /* a     */ {0x20,0x54,0x54,0x54,0x78},
    /* b     */ {0x7F,0x48,0x44,0x44,0x38},
    /* c     */ {0x38,0x44,0x44,0x44,0x20},
    /* d     */ {0x38,0x44,0x44,0x48,0x7F},
    /* e     */ {0x38,0x54,0x54,0x54,0x18},
    /* f     */ {0x08,0x7E,0x09,0x01,0x02},
    /* g     */ {0x0C,0x52,0x52,0x52,0x3E},
    /* h     */ {0x7F,0x08,0x04,0x04,0x78},
    /* i     */ {0x00,0x44,0x7D,0x40,0x00},
    /* j     */ {0x20,0x40,0x44,0x3D,0x00},
    /* k     */ {0x7F,0x10,0x28,0x44,0x00},
    /* l     */ {0x00,0x41,0x7F,0x40,0x00},
    /* m     */ {0x7C,0x04,0x18,0x04,0x78},
    /* n     */ {0x7C,0x08,0x04,0x04,0x78},
    /* o     */ {0x38,0x44,0x44,0x44,0x38},
    /* p     */ {0x7C,0x14,0x14,0x14,0x08},
    /* q     */ {0x08,0x14,0x14,0x18,0x7C},
    /* r     */ {0x7C,0x08,0x04,0x04,0x08},
    /* s     */ {0x48,0x54,0x54,0x54,0x20},
    /* t     */ {0x04,0x3F,0x44,0x40,0x20},
    /* u     */ {0x3C,0x40,0x40,0x20,0x7C},
    /* v     */ {0x1C,0x20,0x40,0x20,0x1C},
    /* w     */ {0x3C,0x40,0x20,0x40,0x3C},
    /* x     */ {0x44,0x28,0x10,0x28,0x44},
    /* y     */ {0x0C,0x50,0x50,0x50,0x3C},
    /* z     */ {0x44,0x64,0x54,0x4C,0x44},
    /* {     */ {0x08,0x36,0x41,0x00,0x00},
    /* |     */ {0x00,0x00,0x7F,0x00,0x00},
    /* }     */ {0x00,0x00,0x41,0x36,0x08},
    /* ~     */ {0x08,0x04,0x08,0x10,0x08},
};

/* ── framebuffers ───────────────────────────────────────────────────────── */
static uint8_t bw_buf[EINK_BUF_SIZE];
static uint8_t red_buf[EINK_BUF_SIZE];

/* Forward decl so lpix() (and helpers below it) can read the effective
   variant — the actual state and definition live further down with the
   rest of the dashboard refresh state. */
static eink_panel_variant_t effective_panel_variant(void);

/* ── landscape pixel primitive ──────────────────────────────────────────── */
/* lx ∈ [0,295], ly ∈ [0,127]. Rotation 90° CW → physical px=ly, py=295-lx
   (the leading-comment "127-ly" in earlier revisions was stale — the code
   below is authoritative). On a BW panel every red operation collapses
   into a BW operation so use_red=1,black=1 → BW black and use_red=1,
   black=0 → BW white (preserves the existing white-hole semantics
   inside red regions). */
static void lpix(int lx, int ly, int black, int use_red)
{
    int px = ly;
    int py = (EINK_HEIGHT - 1) - lx;
    if ((unsigned)px >= EINK_WIDTH || (unsigned)py >= EINK_HEIGHT) return;
    int i = (py * EINK_WIDTH + px) / 8;
    int b = 7 - ((py * EINK_WIDTH + px) % 8);

    if (effective_panel_variant() == EINK_PANEL_WEACT_29_BW) {
        if (black) bw_buf[i] &= ~(1 << b);
        else       bw_buf[i] |=  (1 << b);
        return;
    }

    if (use_red) {
        if (black) red_buf[i] |=  (1 << b);
        else       red_buf[i] &= ~(1 << b);
    } else {
        if (black) bw_buf[i]  &= ~(1 << b);
        else       bw_buf[i]  |=  (1 << b);
    }
}

static void fill_rect(int lx, int ly, int w, int h, int black, int use_red)
{
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++)
            lpix(lx + dx, ly + dy, black, use_red);
}

static void hline(int lx, int ly, int len)
{
    for (int i = 0; i < len; i++) lpix(lx + i, ly, 1, 0);
}

static void vline(int lx, int ly, int len)
{
    for (int i = 0; i < len; i++) lpix(lx, ly + i, 1, 0);
}

/* ── text ───────────────────────────────────────────────────────────────── */
static void draw_char(int lx, int ly, char c, int use_red)
{
    if ((unsigned char)c < 32 || (unsigned char)c > 126) c = '?';
    const uint8_t *g = font5x7[(unsigned char)c - 32];
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 7; row++)
            if (g[col] & (1 << row))
                lpix(lx + col, ly + row, 1, use_red);
}

static void draw_char_bw(int lx, int ly, char c, int black)
{
    if ((unsigned char)c < 32 || (unsigned char)c > 126) c = '?';
    const uint8_t *g = font5x7[(unsigned char)c - 32];
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 7; row++)
            if (g[col] & (1 << row))
                lpix(lx + col, ly + row, black, 0);
}

static void draw_str(int lx, int ly, const char *s, int use_red)
{
    while (*s) { draw_char(lx, ly, *s++, use_red); lx += FONT_W; }
}

static int str_w(const char *s)  { return (int)strlen(s) * FONT_W; }

/* Currency symbols outside ASCII need a standalone 5x7 bitmap (column-major,
   same layout as font5x7) — the font5x7 table is kept to its 95-glyph ASCII
   range. `$` reuses the font glyph; only `€` needs its own bitmap. */
static const uint8_t glyph_euro[5] = { 0x3E, 0x55, 0x55, 0x41, 0x00 };

static void draw_currency_symbol(int lx, int ly, const char *code, int use_red)
{
    if (code && strcmp(code, "EUR") == 0) {
        for (int col = 0; col < 5; col++)
            for (int row = 0; row < 7; row++)
                if (glyph_euro[col] & (1 << row))
                    lpix(lx + col, ly + row, 1, use_red);
        return;
    }
    /* USD and any unknown/empty code fall back to the ASCII dollar glyph. */
    draw_char(lx, ly, '$', use_red);
}

/* Format a spend amount with up to 2 decimals, trailing zeros (and a bare
   trailing dot) trimmed: 0.91 -> "0.91", 1.00 -> "1", 1200 -> "1200". The
   decimal separator follows the currency: EUR uses a comma (NL/eurozone
   convention), everything else keeps the dot. */
static void format_spend_amount(char *dst, size_t dsz, double amount,
                                const char *currency)
{
    if (amount < 0) amount = 0;
    if (amount > 99999) amount = 99999;
    snprintf(dst, dsz, "%.2f", amount);
    char *dot = strchr(dst, '.');
    if (!dot) return;
    char *end = dst + strlen(dst) - 1;
    while (end > dot && *end == '0') *end-- = '\0';
    if (end == dot) { *end = '\0'; return; }
    if (currency && strcmp(currency, "EUR") == 0) *dot = ',';
}

static void draw_str_adv(int lx, int ly, const char *s, int black, int advance)
{
    while (*s) {
        draw_char_bw(lx, ly, *s++, black);
        lx += advance;
    }
}

static void draw_str_clipped_adv(int lx, int ly, const char *s, int black,
                                 int advance, int max_w)
{
    int used = 0;
    while (*s && used + 5 <= max_w) {
        draw_char_bw(lx, ly, *s++, black);
        lx += advance;
        used += advance;
    }
}

static int str_w_adv(const char *s, int advance)
{
    return (int)strlen(s) * advance;
}

/* 2× scaled */
static void draw_char2x(int lx, int ly, char c, int use_red)
{
    if ((unsigned char)c < 32 || (unsigned char)c > 126) c = '?';
    const uint8_t *g = font5x7[(unsigned char)c - 32];
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 7; row++)
            if (g[col] & (1 << row))
                fill_rect(lx + col * 2, ly + row * 2, 2, 2, 1, use_red);
}

static void draw_str2x(int lx, int ly, const char *s, int use_red)
{
    while (*s) { draw_char2x(lx, ly, *s++, use_red); lx += FONT2_W; }
}

static int str2x_w(const char *s) { return (int)strlen(s) * FONT2_W; }

static void draw_char4x_bw(int lx, int ly, char c, int black)
{
    if ((unsigned char)c < 32 || (unsigned char)c > 126) c = '?';
    const uint8_t *g = font5x7[(unsigned char)c - 32];
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 7; row++)
            if (g[col] & (1 << row))
                fill_rect(lx + col * 4, ly + row * 4, 4, 4, black, 0);
}

static void draw_str4x_bw(int lx, int ly, const char *s, int black)
{
    while (*s) {
        draw_char4x_bw(lx, ly, *s++, black);
        lx += 23;
    }
}

static void draw_char_inv(int lx, int ly, char c)
{
    if ((unsigned char)c < 32 || (unsigned char)c > 126) c = '?';
    const uint8_t *g = font5x7[(unsigned char)c - 32];
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            if (g[col] & (1 << row)) {
                lpix(lx + col, ly + row, 0, 0);
                lpix(lx + col, ly + row, 0, 1);
            }
        }
    }
}

static void draw_str_inv_adv(int lx, int ly, const char *s, int advance)
{
    while (*s) {
        draw_char_inv(lx, ly, *s++);
        lx += advance;
    }
}

static void draw_char4x_inv(int lx, int ly, char c)
{
    if ((unsigned char)c < 32 || (unsigned char)c > 126) c = '?';
    const uint8_t *g = font5x7[(unsigned char)c - 32];
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            if (g[col] & (1 << row)) {
                fill_rect(lx + col * 4, ly + row * 4, 4, 4, 0, 0);
                fill_rect(lx + col * 4, ly + row * 4, 4, 4, 0, 1);
            }
        }
    }
}

static void draw_str4x_inv(int lx, int ly, const char *s)
{
    while (*s) {
        draw_char4x_inv(lx, ly, *s++);
        lx += 23;
    }
}

static void draw_char3x_inv(int lx, int ly, char c)
{
    if ((unsigned char)c < 32 || (unsigned char)c > 126) c = '?';
    const uint8_t *g = font5x7[(unsigned char)c - 32];
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            if (g[col] & (1 << row)) {
                fill_rect(lx + col * 3, ly + row * 3, 3, 3, 0, 0);
                fill_rect(lx + col * 3, ly + row * 3, 3, 3, 0, 1);
            }
        }
    }
}

static void draw_str3x_inv(int lx, int ly, const char *s)
{
    while (*s) {
        draw_char3x_inv(lx, ly, *s++);
        lx += 17;
    }
}

static void fit_text_ascii(char *out, size_t out_sz, const char *in, int max_w)
{
    if (out_sz == 0) return;
    if (!in) in = "";

    size_t max_chars = max_w > 0 ? (size_t)(max_w / FONT_BOOT_W) : 0;
    size_t len = strlen(in);
    if (len <= max_chars && len < out_sz) {
        memcpy(out, in, len + 1);
        return;
    }

    if (max_chars == 0) {
        out[0] = '\0';
        return;
    }

    size_t keep = max_chars;
    if (keep >= out_sz) keep = out_sz - 1;
    if (keep > 3) keep -= 3;
    memcpy(out, in, keep);
    out[keep] = '\0';
    if (max_chars > 3 && keep + 3 < out_sz) {
        memcpy(out + keep, "...", 4);
    }
}

/* ── pixel icons ────────────────────────────────────────────────────────── */

static void icon_box_logo(int ox, int oy)
{
    fill_rect(ox,   oy,   9, 9, 1, 0);  /* 9×9 solid black  */
    fill_rect(ox+2, oy+2, 5, 5, 0, 0);  /* 5×5 white cutout */
    fill_rect(ox+3, oy+3, 3, 3, 1, 0);  /* 3×3 black centre */
}

static void icon_down_arrow(int ox, int oy)
{
    static const int8_t pts[] = {
        3,0, 3,1, 3,2, 3,3, 3,4, 3,5,
        1,4, 2,5, 3,6, 4,5, 5,4,
    };
    for (size_t i = 0; i < sizeof(pts) / sizeof(pts[0]) / 2; i++) {
        lpix(ox + pts[i * 2], oy + pts[i * 2 + 1], 1, 0);
    }
}

/* Issue icon: bordered circle with centre dot */
static void icon_issue(int ox, int oy, int use_red)
{
    fill_rect(ox+3, oy+0, 4, 1, 1, use_red);
    fill_rect(ox+2, oy+1, 6, 1, 1, use_red);
    fill_rect(ox+1, oy+2, 2, 6, 1, use_red);
    fill_rect(ox+7, oy+2, 2, 6, 1, use_red);
    fill_rect(ox+2, oy+8, 6, 1, 1, use_red);
    fill_rect(ox+3, oy+9, 4, 1, 1, use_red);
    fill_rect(ox+4, oy+4, 2, 2, 1, use_red);
}

/* PR icon: git branch glyph */
static void icon_pr(int ox, int oy, int use_red)
{
    fill_rect(ox+1, oy+0, 2, 2, 1, use_red);
    fill_rect(ox+1, oy+8, 2, 2, 1, use_red);
    fill_rect(ox+7, oy+0, 2, 2, 1, use_red);
    fill_rect(ox+1, oy+2, 1, 6, 1, use_red);
    fill_rect(ox+8, oy+2, 1, 3, 1, use_red);
    fill_rect(ox+4, oy+5, 4, 1, 1, use_red);
    fill_rect(ox+4, oy+4, 1, 2, 1, use_red);
}

/* Inbox icon: tray with a single unread notch. */
static void icon_inbox(int ox, int oy, int use_red)
{
    fill_rect(ox+1, oy+1, 8, 1, 1, use_red);
    fill_rect(ox+0, oy+2, 1, 6, 1, use_red);
    fill_rect(ox+9, oy+2, 1, 6, 1, use_red);
    fill_rect(ox+1, oy+7, 8, 1, 1, use_red);
    fill_rect(ox+2, oy+5, 2, 1, 1, use_red);
    fill_rect(ox+6, oy+5, 2, 1, 1, use_red);
    fill_rect(ox+4, oy+6, 2, 1, 1, use_red);
}

/* Compact GitHub mark for the left column title. */
static void icon_github_mark(int ox, int oy, int use_red)
{
    fill_rect(ox+2, oy+1, 6, 1, 1, use_red);
    fill_rect(ox+1, oy+2, 8, 4, 1, use_red);
    fill_rect(ox+2, oy+6, 6, 2, 1, use_red);
    fill_rect(ox+3, oy+8, 1, 2, 1, use_red);
    fill_rect(ox+6, oy+8, 1, 2, 1, use_red);
    fill_rect(ox+3, oy+4, 1, 1, 0, 0);
    fill_rect(ox+6, oy+4, 1, 1, 0, 0);
}

/* Shield icon — RED alert cuts an "!" hole in white */
static void icon_shield(int ox, int oy, int use_red)
{
    fill_rect(ox+2, oy+0, 6, 1, 1, use_red);
    fill_rect(ox+1, oy+1, 8, 5, 1, use_red);
    fill_rect(ox+2, oy+6, 6, 1, 1, use_red);
    fill_rect(ox+3, oy+7, 4, 1, 1, use_red);
    fill_rect(ox+4, oy+8, 2, 1, 1, use_red);
    if (use_red) {
        /* "!" hole: white on both planes */
        fill_rect(ox+4, oy+2, 2, 2, 0, 0);
        fill_rect(ox+4, oy+2, 2, 2, 0, 1);
        fill_rect(ox+4, oy+5, 2, 1, 0, 0);
        fill_rect(ox+4, oy+5, 2, 1, 0, 1);
    }
}

/* Key icon — bow with shaft and two teeth */
static void icon_key(int ox, int oy, int use_red)
{
    fill_rect(ox+0, oy+3, 3, 3, 1, use_red);
    /* bow hole: white on bw plane */
    fill_rect(ox+1, oy+4, 1, 1, 0, 0);
    if (use_red) fill_rect(ox+1, oy+4, 1, 1, 0, 1);
    fill_rect(ox+3, oy+4, 6, 1, 1, use_red);
    fill_rect(ox+6, oy+5, 1, 2, 1, use_red);
    fill_rect(ox+8, oy+5, 1, 2, 1, use_red);
}

/* Hourglass icon (7×7) — used in provider title rows next to reset countdown. */
static void icon_hourglass(int ox, int oy, int use_red)
{
    fill_rect(ox+0, oy+0, 7, 1, 1, use_red);
    fill_rect(ox+1, oy+1, 5, 1, 1, use_red);
    fill_rect(ox+2, oy+2, 3, 1, 1, use_red);
    fill_rect(ox+3, oy+3, 1, 1, 1, use_red);
    fill_rect(ox+2, oy+4, 3, 1, 1, use_red);
    fill_rect(ox+1, oy+5, 5, 1, 1, use_red);
    fill_rect(ox+0, oy+6, 7, 1, 1, use_red);
}

/* Sync glyph: two open arrows forming a refresh circle */
static void icon_sync(int ox, int oy)
{
    fill_rect(ox+2, oy+1, 5, 1, 1, 0);
    fill_rect(ox+1, oy+2, 1, 3, 1, 0);
    fill_rect(ox+6, oy+0, 1, 3, 1, 0);
    fill_rect(ox+2, oy+7, 5, 1, 1, 0);
    fill_rect(ox+7, oy+4, 1, 3, 1, 0);
    fill_rect(ox+2, oy+6, 1, 3, 1, 0);
}

/* Moon glyph (8×8 crescent). black=1 draws black pixels (header, on white);
   black=0 draws white pixels (sleeping footer, on the black bar). */
static void icon_moon(int ox, int oy, int black)
{
    static const uint8_t rows[8] = {
        0x3C, 0x0E, 0x07, 0x07, 0x07, 0x07, 0x0E, 0x3C,
    };
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            if (rows[row] & (1 << col)) lpix(ox + col, oy + row, black, 0);
        }
    }
}

/* Cross-sync glyph: X mark in RED */
static void icon_cross_sync(int ox, int oy)
{
    for (int i = 0; i < 8; i++) lpix(ox + i, oy + i, 1, 1);
    static const int8_t d2[] = {0,7, 1,6, 2,5, 3,4, 5,3, 6,2, 7,1};
    for (int i = 0; i < 7; i++) lpix(ox + d2[i*2], oy + d2[i*2+1], 1, 1);
}

/* WiFi icon (10×10) — three arcs + dot. V4 README §S1 icon bitmaps. */
static void icon_wifi(int ox, int oy)
{
    fill_rect(ox+2, oy+2, 6, 1, 1, 0);
    fill_rect(ox+1, oy+3, 1, 1, 1, 0);
    fill_rect(ox+8, oy+3, 1, 1, 1, 0);
    fill_rect(ox+2, oy+5, 1, 1, 1, 0);
    fill_rect(ox+3, oy+5, 4, 1, 1, 0);
    fill_rect(ox+7, oy+5, 1, 1, 1, 0);
    fill_rect(ox+4, oy+7, 2, 1, 1, 0);
    fill_rect(ox+4, oy+9, 2, 1, 1, 0);
}

static void icon_arrow_right(int ox, int oy)
{
    hline(ox, oy + 3, 8);
    lpix(ox + 6, oy + 1, 1, 0);
    lpix(ox + 7, oy + 2, 1, 0);
    lpix(ox + 8, oy + 3, 1, 0);
    lpix(ox + 7, oy + 4, 1, 0);
    lpix(ox + 6, oy + 5, 1, 0);
}

static void draw_header_connection_slots(void)
{
    if (!s_header_slots.valid || s_header_slots.wifi_slot == 0) return;

    const int icon_w = 10;
    const int arrow_w = 9;
    const int gap = 3;
    char wifi_slot[2] = { (char)('0' + s_header_slots.wifi_slot), '\0' };
    char api_slot[2] = { (char)('0' + s_header_slots.active_api_slot), '\0' };
    int width = icon_w + gap + str_w(wifi_slot);
    if (s_header_slots.active_api_slot != 0) {
        width += gap + arrow_w + gap + str_w(api_slot);
    }

    int x = (296 - width) / 2;
    const int icon_y = 3;
    const int arrow_y = 4;
    const int text_y = 4;

    icon_wifi(x, icon_y);
    x += icon_w + gap;
    draw_str(x, text_y, wifi_slot, 0);

    if (s_header_slots.active_api_slot == 0) return;

    x += str_w(wifi_slot) + gap;
    icon_arrow_right(x, arrow_y);
    x += arrow_w + gap;
    draw_str(x, text_y, api_slot, 0);
}

/* Globe icon (10×10) — ringed circle with one latitude + one meridian. */
static void icon_globe(int ox, int oy)
{
    /* outer ring */
    fill_rect(ox+3, oy+0, 4, 1, 1, 0);
    fill_rect(ox+2, oy+1, 6, 1, 1, 0);
    fill_rect(ox+1, oy+2, 2, 6, 1, 0);
    fill_rect(ox+7, oy+2, 2, 6, 1, 0);
    fill_rect(ox+2, oy+8, 6, 1, 1, 0);
    fill_rect(ox+3, oy+9, 4, 1, 1, 0);
    /* horizontal latitudes */
    fill_rect(ox+2, oy+3, 6, 1, 1, 0);
    fill_rect(ox+2, oy+6, 6, 1, 1, 0);
    /* vertical meridian with white cuts at the horizontals */
    fill_rect(ox+4, oy+1, 2, 8, 1, 0);
    fill_rect(ox+4, oy+2, 2, 1, 0, 0);
    fill_rect(ox+4, oy+4, 2, 2, 0, 0);
    fill_rect(ox+4, oy+7, 2, 1, 0, 0);
}

/* Big-X (19×19) error glyph, RED. Two diagonals share (9,9). */
__attribute__((unused))
static void icon_big_cross_red(int ox, int oy)
{
    for (int i = 0; i < 19; i++) {
        lpix(ox + i,        oy + i,        1, 1);
        if (i != 9) lpix(ox + i, oy + 18 - i, 1, 1);
    }
}

/* ── segmented bar ──────────────────────────────────────────────────────── */
/* Filled segments past the 80% threshold render red when pct > 80. Empty
 * segments are hollow 1-px black boxes. */
static void draw_bar_cfg_ex(int ox, int oy, int width, int height, int seg_w,
                            int pct, bool force_red)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int stride = seg_w + 1;
    int cols = (width + 1) / stride;
    int filled = (pct * cols + 50) / 100;
    int thresh = (80 * cols + 50) / 100;
    for (int i = 0; i < cols; i++) {
        int sx = ox + i * stride;
        if (i < filled) {
            int is_red = force_red || ((pct > 80) && (i >= thresh));
            fill_rect(sx, oy, seg_w, height, 1, is_red);
        } else {
            /* outlined empty box: 1 px black border, white interior */
            hline(sx, oy, seg_w);
            hline(sx, oy + height - 1, seg_w);
            vline(sx, oy + 1, height - 2);
            vline(sx + seg_w - 1, oy + 1, height - 2);
        }
    }
}

static void draw_bar_cfg(int ox, int oy, int width, int height, int seg_w, int pct)
{
    draw_bar_cfg_ex(ox, oy, width, height, seg_w, pct, false);
}

/* Format remaining seconds as a compact countdown string ("0h47", "2h14",
 * "2d 9h", "5d12h", "10+d", "--"). Buffer must be at least 6 bytes. */
static void format_reset_countdown(char *out, size_t out_sz, int seconds)
{
    if (seconds <= 0) { snprintf(out, out_sz, "--"); return; }
    int total_min = seconds / 60;
    if (total_min < 60) {
        snprintf(out, out_sz, "0h%02d", total_min);
        return;
    }
    if (total_min < 24 * 60) {
        int h = total_min / 60;
        int m = total_min % 60;
        snprintf(out, out_sz, "%dh%02d", h, m);
        return;
    }
    int total_hours = total_min / 60;
    int d = total_hours / 24;
    int h = total_hours % 24;
    if (d > 9) { snprintf(out, out_sz, "10+d"); return; }
    if (h < 10) snprintf(out, out_sz, "%dd %dh", d, h);
    else        snprintf(out, out_sz, "%dd%dh", d, h);
}

typedef enum {
    PROVIDER_LOGO_SPARK,
    PROVIDER_LOGO_RING,
    PROVIDER_LOGO_LIFT,
    PROVIDER_LOGO_DIAMOND,
    PROVIDER_LOGO_COUNT_,  /* sentinel — add a row to rows[] in icon_provider_logo when extending */
} provider_logo_t;

typedef struct {
    const char *label;
    provider_logo_t logo;
    const char *ses_label;
    const char *wk_label;
    int ses;
    int wk;
    int ses_reset;
    int wk_reset;
    const extra_usage_t *extra;
    bool auth_err;
    bool reached;
} provider_info_t;

static provider_logo_t provider_logo_from_name(const char *name)
{
    if (name && strcmp(name, "spark") == 0) return PROVIDER_LOGO_SPARK;
    if (name && strcmp(name, "ring") == 0) return PROVIDER_LOGO_RING;
    if (name && strcmp(name, "lift") == 0) return PROVIDER_LOGO_LIFT;
    return PROVIDER_LOGO_DIAMOND;
}

static void icon_provider_logo(int ox, int oy, provider_logo_t logo,
                               int scale, int use_red)
{
    static const uint8_t rows[][7] = {
        { 0x49, 0x2A, 0x1C, 0x77, 0x1C, 0x2A, 0x49 },
        { 0x1C, 0x22, 0x41, 0x41, 0x41, 0x22, 0x1C },
        { 0x08, 0x00, 0x08, 0x14, 0x22, 0x41, 0x7F },
        { 0x08, 0x14, 0x22, 0x41, 0x22, 0x14, 0x08 },
    };
    _Static_assert(sizeof(rows)/sizeof(rows[0]) == PROVIDER_LOGO_COUNT_,
                   "rows[] must have one entry per logo; add a row when extending provider_logo_t");
    const uint8_t *bitmap = rows[logo];
    for (int y = 0; y < 7; y++) {
        for (int x = 0; x < 7; x++) {
            if (bitmap[y] & (1u << (6 - x))) {
                fill_rect(ox + x * scale, oy + y * scale,
                          scale, scale, 1, use_red);
            }
        }
    }
}

static void provider_reset_strings(const provider_info_t *provider,
                                   char *ses, size_t ses_sz,
                                   char *wk, size_t wk_sz)
{
    if (provider->auth_err) {
        snprintf(ses, ses_sz, "--");
        snprintf(wk, wk_sz, "--");
        return;
    }
    format_reset_countdown(ses, ses_sz, provider->ses_reset);
    format_reset_countdown(wk, wk_sz, provider->wk_reset);
}

static void format_reset_pair(const provider_info_t *provider, char *out, size_t out_sz)
{
    char ses[8], wk[8];
    provider_reset_strings(provider, ses, sizeof(ses), wk, sizeof(wk));
    snprintf(out, out_sz, "%s/%s", ses, wk);
}

static void draw_provider_title(int ox, int oy, int width,
                                const provider_info_t *provider,
                                bool show_hourglass)
{
    char resets[20];
    format_reset_pair(provider, resets, sizeof(resets));

    icon_provider_logo(ox, oy, provider->logo, 1, provider->auth_err);
    draw_str(ox + 11, oy, provider->label, provider->auth_err);

    int reset_x = ox + width - str_w(resets);
    if (show_hourglass) {
        int hourglass_x = reset_x - 10;
        icon_hourglass(hourglass_x, oy, provider->auth_err);
    }
    draw_str(reset_x, oy, resets,
             provider->auth_err || provider->ses > 80 || provider->wk > 80);
}

static void draw_provider_percent_row(int ox, int oy, int width,
                                      const char *label, int pct,
                                      int label_w,
                                      int bar_h, int seg_w,
                                      bool auth_err)
{
    char value[8];
    snprintf(value, sizeof(value), auth_err ? "ERR" : "%d%%", pct);
    int bar_x = ox + label_w;
    int value_x = ox + width - str_w(value);
    /* Anchor the bar's right edge at the position where "100%" would sit so
       all percentage bars are the same width regardless of digit count.
       Shrink only when the actual value text would overlap that fixed edge. */
    int fixed_end_x = ox + width - str_w("100%") - 2;
    int bar_end_x = (value_x - 2 < fixed_end_x) ? value_x - 2 : fixed_end_x;
    int bar_w = bar_end_x - bar_x;
    if (bar_w < 0) bar_w = 0;

    draw_str(ox, oy, label, auth_err);
    draw_bar_cfg(bar_x, oy, bar_w, bar_h, seg_w, auth_err ? 0 : pct);
    draw_str(value_x, oy, value, auth_err || pct > 80);
}

static void draw_provider_extra_row(int ox, int oy, int width,
                                    const extra_usage_t *extra,
                                    int label_w, int bar_h, int seg_w,
                                    bool auth_err)
{
    if (!extra || !extra->present) return;

    char amount[16];
    if (extra->value_text[0] != '\0') {
        snprintf(amount, sizeof(amount), "%s", extra->value_text);
    } else {
        format_spend_amount(amount, sizeof(amount), extra->amount,
                            extra->currency);
    }

    int pct;
    if (extra->percent_present) {
        pct = extra->percent;
    } else {
        int rounded = (int)(extra->amount + 0.5);
        pct = rounded < 0 ? 0 : (rounded > 100 ? 100 : rounded);
    }

    int bar_x = ox + label_w;
    int amount_x = ox + width - str_w(amount);
    int bar_w = amount_x - 2 - bar_x;
    if (bar_w < 0) bar_w = 0;
    draw_currency_symbol(ox, oy, extra->currency, auth_err);
    draw_bar_cfg_ex(bar_x, oy, bar_w, bar_h, seg_w, pct,
                    extra->amount > 0);
    draw_str(amount_x, oy, amount, auth_err || extra->amount > 0);
}

static void draw_provider_grid(int ox, int oy, int width,
                               const provider_info_t *provider)
{
    draw_provider_title(ox, oy, width, provider, false);
    draw_provider_percent_row(ox, oy + 13, width, provider->ses_label, provider->ses,
                              15, 6, 3, provider->auth_err);
    draw_provider_percent_row(ox, oy + 22, width, provider->wk_label, provider->wk,
                              15, 6, 3, provider->auth_err);
    draw_provider_extra_row(ox, oy + 31, width, provider->extra,
                            15, 6, 3, provider->auth_err);
}

static void draw_provider_row(int ox, int oy, int width,
                              const provider_info_t *provider)
{
    draw_provider_title(ox, oy, width, provider, true);
    draw_provider_percent_row(ox, oy + 14, width, provider->ses_label, provider->ses,
                              18, 8, 3, provider->auth_err);
    draw_provider_percent_row(ox, oy + 23, width, provider->wk_label, provider->wk,
                              18, 8, 3, provider->auth_err);
    draw_provider_extra_row(ox, oy + 32, width, provider->extra,
                            18, 8, 3, provider->auth_err);
}

static void draw_provider_hero(int ox, int oy, int width,
                               const provider_info_t *provider)
{
    char resets[20];
    format_reset_pair(provider, resets, sizeof(resets));

    icon_provider_logo(ox, oy, provider->logo, 3, provider->auth_err);
    draw_str2x(ox + 29, oy + 3, provider->label, provider->auth_err);
    int reset_x = ox + width - str_w(resets);
    icon_hourglass(reset_x - 10, oy + 7, provider->auth_err);
    draw_str(reset_x, oy + 7, resets,
             provider->auth_err || provider->ses > 80 || provider->wk > 80);

    draw_provider_percent_row(ox, oy + 28, width, provider->ses_label, provider->ses,
                              26, 12, 4, provider->auth_err);
    draw_provider_percent_row(ox, oy + 47, width, provider->wk_label, provider->wk,
                              26, 12, 4, provider->auth_err);
    draw_provider_extra_row(ox, oy + 66, width, provider->extra,
                            26, 12, 4, provider->auth_err);
}

static void draw_github_strip_chip(int icon_x,
                                   void (*icon)(int,int,int),
                                   const char *label,
                                   const char *value,
                                   int value_right,
                                   bool use_red)
{
    icon(icon_x, 18, use_red);
    draw_str(icon_x + 14, 20, label, 0);
    draw_str(value_right - str_w(value), 20, value, use_red);
}

static void format_count_value(char *out, size_t out_sz, int value)
{
    if (value > 999) {
        snprintf(out, out_sz, "999+");
    } else {
        snprintf(out, out_sz, "%d", value < 0 ? 0 : value);
    }
}

static void draw_github_strip(const github_data_t *github)
{
    char issues[8], prs[8], inbox[8], deps[8];
    bool error = github->auth_error || github->service_error;

    icon_github_mark(5, 18, error);
    draw_str(19, 20, "GH", error);
    if (error) {
        const char *label = github->auth_error ? "AUTH FAIL" : "OFFLINE";
        draw_str(290 - str_w(label), 20, label, 1);
        hline(2, 32, 292);
        return;
    }

    format_count_value(issues, sizeof(issues), github->issues);
    format_count_value(prs, sizeof(prs), github->prs);
    format_count_value(inbox, sizeof(inbox), github->notifications);
    if (github->dependabot > 0) {
        format_count_value(deps, sizeof(deps), github->dependabot);
        strlcat(deps, "!", sizeof(deps));
    } else {
        format_count_value(deps, sizeof(deps), github->dependabot);
    }

    draw_github_strip_chip(38, icon_issue, "ISS", issues, 94, false);
    draw_github_strip_chip(97, icon_pr, "PR", prs, 148, false);
    if (github->notifications_present) {
        draw_github_strip_chip(151, icon_inbox, "INBOX", inbox, 226, false);
    }
    draw_github_strip_chip(229, icon_shield, "DEP", deps, 291,
                           github->dependabot > 0);
    hline(2, 32, 292);
}

/* ── device handle ──────────────────────────────────────────────────────── */
static eink_handle_t s_eink;
static bool          s_initialized        = false;

/* Refresh-cycle state lives in RTC slow memory: survives deep-sleep timer
 * wakeups and software restarts (so differential refresh stays valid across
 * naps), but resets on cold boot / power loss where panel state is undefined.
 * DISPLAY_RTC_STATE_MAGIC invalidates this cache when its in-memory ABI changes
 * across an OTA. */
static RTC_DATA_ATTR bool    s_first_refresh_done = false;
static RTC_DATA_ATTR bool    s_last_red_state      = false;
static RTC_DATA_ATTR bool    s_last_content_valid  = false;
static RTC_DATA_ATTR bool    s_last_bw_valid       = false;
static RTC_DATA_ATTR bool    s_last_data_valid     = false;
#define DISPLAY_RTC_STATE_MAGIC 0x44563432u
/* Keep this marker before dashboard_data_t. When that struct changes across an
   OTA, this address previously held its schema_version field, so the new magic
   cannot accidentally validate the old RTC layout. Change the magic whenever
   the cached data shape or partial-refresh region contract changes. */
static RTC_DATA_ATTR uint32_t s_display_rtc_state_magic;
static RTC_DATA_ATTR dashboard_data_t s_last_data;
static RTC_DATA_ATTR uint8_t s_last_bw_buf[EINK_BUF_SIZE];
static int64_t s_last_full_refresh_us = 0;

/* Quiet-hours sleeping overlay. s_sleeping_mode gates the sleeping-specific
   header clock and footer bar inside draw_dashboard_frame; s_wake_hhmm holds the
   "WAKES HH:MM" string. s_force_full_next_render is RTC-persisted so the first
   dashboard render AFTER the window forces a full refresh — the footer bar sits
   below the per-region partial-refresh rectangles, so only a full refresh is
   guaranteed to clear it. */
static bool s_sleeping_mode = false;
static char s_wake_hhmm[6] = {0};
static RTC_DATA_ATTR bool s_force_full_next_render = false;

/* BW per-region partial cap: how many partials a region may take before it is
   forced to a full refresh. Portal-configurable (cfg.max_partials); main.c sets
   it via display_set_max_partials() each boot before the first render. Defaults
   to DASH_MAX_PARTIALS_DEFAULT until set. */
static uint8_t s_max_partials_per_region = DASH_MAX_PARTIALS_DEFAULT;

#define DISPLAY_META_NAMESPACE       "disp_meta"
#define DISPLAY_META_KEY             "state"
#define DISPLAY_META_KEY_LAST_VAR    "last_var"
#define DISPLAY_META_MAGIC           0xD15DA5E1u

/* Panel variant state. effective_panel_variant() returns BW until
   display_set_panel_variant() is called; main.c calls it before the first
   draw with the persisted config or, on an erased-NVS device, the build's
   default variant (CONFIG_DEVDASH_DEFAULT_PANEL_VARIANT, BWR in the repo
   build), so the variant is known before any surface is rendered. */
static eink_panel_variant_t s_panel_variant = EINK_PANEL_WEACT_29_BWR;
static bool                 s_variant_known = false;
static uint8_t              s_refresh_min   = CONFIG_DEVDASH_REFRESH_MIN;

/* Per-region partial machinery (BW path). The active V4 layouts use at most six
   entries. Keep the historical capacity of seven so OTA does not shift the RTC
   variables that follow this array. */
#define REGION_COUNT_MAX 7
static RTC_DATA_ATTR uint8_t  s_region_partial_count[REGION_COUNT_MAX];
static RTC_DATA_ATTR uint8_t  s_offline_partial_count;
static RTC_DATA_ATTR bool     s_force_full_refresh          = false;
static RTC_DATA_ATTR uint16_t s_renders_since_full          = 0;
/* The per-region partial machinery uses these to detect a show_github
   layout flip between renders: the active region set and indices change
   with the layout, so a flip forces a full refresh before the new layout
   is committed. */
static RTC_DATA_ATTR bool     s_last_dashboard_show_github  = false;
static RTC_DATA_ATTR bool     s_last_dashboard_layout_valid = false;

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Per-region partial table. Each region is declared in logical (296x128
   landscape) coordinates — natural to read against the dashboard layout —
   and converted to a physical, byte-aligned framebuffer rect by
   physical_rect_from_logical() (which uses the exact lpix() transform).
   Each region's ly start/end are multiples of 8 so the derived physical-X
   byte ranges are disjoint between regions sharing a physical-Y band. */
typedef struct {
    int lx, ly, lw, lh;
} region_logical_t;

/* V4 GitHub-strip layout. The four provider cells remain separate partial
   regions so an unchanged quota group does not need another panel update. */
static const region_logical_t s_regions_with_github[] = {
    {   0,  0, 296, 16 },  /* header */
    {   0, 16, 296, 16 },  /* GitHub strip */
    {   0, 32, 148, 48 },  /* provider top-left */
    { 148, 32, 148, 48 },  /* provider top-right */
    {   0, 80, 148, 48 },  /* provider bottom-left */
    { 148, 80, 148, 48 },  /* provider bottom-right */
};

/* No-GitHub layout uses the same 2x2 provider grid with taller rows. */
static const region_logical_t s_regions_no_github[] = {
    {   0,  0, 296, 16 },  /* header */
    {   0, 16, 148, 56 },  /* provider top-left */
    { 148, 16, 148, 56 },  /* provider top-right */
    {   0, 72, 148, 56 },  /* provider bottom-left */
    { 148, 72, 148, 56 },  /* provider bottom-right */
};

static eink_panel_variant_t effective_panel_variant(void)
{
    return s_variant_known ? s_panel_variant : EINK_PANEL_WEACT_29_BW;
}

static uint16_t render_count_cap(uint8_t refresh_min)
{
    if (refresh_min < DASH_REFRESH_MIN_BW_TWO_PARTIALS) {
        refresh_min = DASH_REFRESH_MIN_BW_TWO_PARTIALS;
    }
    if (refresh_min > DASH_REFRESH_MAX) refresh_min = DASH_REFRESH_MAX;
    uint16_t raw = (uint16_t)((24u * 60u + refresh_min - 1u) / refresh_min);
    if (raw < 8)   raw = 8;
    if (raw > 1440) raw = 1440;
    return raw;
}

#define HEADER_STATUS_X 220
#define HEADER_STATUS_Y 2
#define HEADER_STATUS_W 74
#define HEADER_STATUS_H 13

typedef enum {
    DISPLAY_FRAME_UNKNOWN = 0,
    DISPLAY_FRAME_CONTENT = 1,
    DISPLAY_FRAME_OFFLINE_API = 2,
    DISPLAY_FRAME_QR      = 3,
    DISPLAY_FRAME_OFFLINE_WIFI = 4,
    DISPLAY_FRAME_OFFLINE_SETUP_TIMEOUT = 5,
    DISPLAY_FRAME_CONNECTING = 6,
    DISPLAY_FRAME_OTA = 7,
} display_frame_t;

typedef struct {
    uint32_t magic;
    uint8_t frame;
    uint8_t reserved[3];
} display_meta_t;

static bool display_meta_load(display_meta_t *meta)
{
    nvs_handle_t h;
    if (nvs_open(DISPLAY_META_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(*meta);
    esp_err_t err = nvs_get_blob(h, DISPLAY_META_KEY, meta, &len);
    nvs_close(h);
    return err == ESP_OK && len == sizeof(*meta) &&
           meta->magic == DISPLAY_META_MAGIC;
}

static void display_mark_frame(display_frame_t frame)
{
    display_meta_t current = {0};
    if (display_meta_load(&current) && current.frame == frame) {
        return;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(DISPLAY_META_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display metadata open failed: %s", esp_err_to_name(err));
        return;
    }
    display_meta_t next = {
        .magic = DISPLAY_META_MAGIC,
        .frame = (uint8_t)frame,
    };
    err = nvs_set_blob(h, DISPLAY_META_KEY, &next, sizeof(next));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display metadata save failed: %s", esp_err_to_name(err));
    }
}

/* Demo builds (CONFIG_DEVDASH_DEMO_MODE) return before nvs_flash_init(),
   so the meta wrappers must tolerate ESP_ERR_NVS_NOT_INITIALIZED quietly
   instead of warning on every render. */
static void log_meta_open_err(const char *key, esp_err_t err)
{
    if (err == ESP_ERR_NVS_NOT_INITIALIZED) {
        ESP_LOGD(TAG, "%s: NVS not initialized (demo build?)", key);
    } else {
        ESP_LOGW(TAG, "%s open: %s", key, esp_err_to_name(err));
    }
}

static bool disp_meta_get_last_var(uint8_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(DISPLAY_META_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) { log_meta_open_err("last_var", err); return false; }
    err = nvs_get_u8(h, DISPLAY_META_KEY_LAST_VAR, out);
    nvs_close(h);
    return err == ESP_OK;
}

static void disp_meta_set_last_var(uint8_t v)
{
    /* No-op guard: this is called on every full refresh (i.e. every wake on
       BWR, which always full-refreshes), but the panel variant only changes
       across a deliberate variant toggle. Skip the open/write/commit entirely
       when the stored value already matches to avoid needless flash wear on
       the disp_meta namespace. */
    uint8_t cur = 0;
    if (disp_meta_get_last_var(&cur) && cur == v) return;

    nvs_handle_t h;
    esp_err_t err = nvs_open(DISPLAY_META_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) { log_meta_open_err("last_var", err); return; }
    err = nvs_set_u8(h, DISPLAY_META_KEY_LAST_VAR, v);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "last_var save: %s", esp_err_to_name(err));
}

/* Last full-refresh wall-clock time lives in RTC slow memory, not NVS. It only
   needs to survive deep-sleep timer wakes — a cold boot / power loss forces a
   full refresh anyway via s_first_refresh_done — so it never has to touch flash.
   Before quiet hours, no trusted clock existed (wall_time_trusted() was always
   false), so disp_meta_set_last_full() never ran; now that timekeep sets the RTC
   clock it fires on every full refresh, which would churn the NVS disp_meta
   namespace if this were still flash-backed. 0 = unset. */
static RTC_DATA_ATTR uint32_t s_last_full_wall_s = 0;

__attribute__((unused))
static bool disp_meta_get_last_full(uint32_t *out)
{
    if (s_last_full_wall_s == 0) return false;
    if (out) *out = s_last_full_wall_s;
    return true;
}

static void disp_meta_set_last_full(uint32_t v)
{
    s_last_full_wall_s = v;
}

static void validate_display_rtc_state(void)
{
    if (s_display_rtc_state_magic == DISPLAY_RTC_STATE_MAGIC) return;

    ESP_LOGI(TAG, "Display RTC state layout changed; invalidating refresh cache");
    s_first_refresh_done = false;
    s_last_red_state = false;
    s_last_content_valid = false;
    s_last_bw_valid = false;
    s_last_data_valid = false;
    memset(&s_last_data, 0, sizeof(s_last_data));
    memset(s_last_bw_buf, 0, sizeof(s_last_bw_buf));
    s_force_full_next_render = false;
    memset(s_region_partial_count, 0, sizeof(s_region_partial_count));
    s_offline_partial_count = 0;
    s_force_full_refresh = false;
    s_renders_since_full = 0;
    s_last_dashboard_show_github = false;
    s_last_dashboard_layout_valid = false;
    s_last_full_wall_s = 0;
    s_display_rtc_state_magic = DISPLAY_RTC_STATE_MAGIC;
}

void display_force_full_refresh_next(void)
{
    s_force_full_refresh = true;
}

void display_set_refresh_min(uint8_t refresh_min)
{
    if (refresh_min < DASH_REFRESH_MIN_BW_TWO_PARTIALS) {
        refresh_min = DASH_REFRESH_MIN_BW_TWO_PARTIALS;
    }
    if (refresh_min > DASH_REFRESH_MAX) refresh_min = DASH_REFRESH_MAX;
    s_refresh_min = refresh_min;
}

void display_set_max_partials(uint8_t max_partials)
{
    if (max_partials < DASH_MAX_PARTIALS_MIN) max_partials = DASH_MAX_PARTIALS_MIN;
    if (max_partials > DASH_MAX_PARTIALS_MAX) max_partials = DASH_MAX_PARTIALS_MAX;
    s_max_partials_per_region = max_partials;
}

void display_set_panel_variant(eink_panel_variant_t v)
{
    validate_display_rtc_state();

    const eink_panel_variant_t old_raw = s_panel_variant;
    const bool                 was_known = s_variant_known;
    const eink_panel_variant_t old_eff =
        was_known ? old_raw : EINK_PANEL_WEACT_29_BW;
    const eink_panel_variant_t new_eff = v;

    s_panel_variant = v;
    s_variant_known = true;

    if (s_initialized && new_eff != old_eff) {
        /* Live-update the driver handle. The SPI bus + GPIOs are already up;
           the next refresh's reset_controller_* will reload the right
           waveform for free. */
        s_eink.variant = new_eff;
        s_eink.asleep  = true;
        display_force_full_refresh_next();
    }
}

static bool display_should_skip_offline_refresh(display_frame_t frame)
{
    esp_reset_reason_t reset = esp_reset_reason();
    if (reset == ESP_RST_POWERON || reset == ESP_RST_BROWNOUT) {
        return false;
    }

    display_meta_t meta = {0};
    return display_meta_load(&meta) && meta.frame == frame;
}

static display_frame_t offline_frame_for_reason(display_offline_reason_t reason)
{
    switch (reason) {
    case DISPLAY_OFFLINE_REASON_WIFI:
        return DISPLAY_FRAME_OFFLINE_WIFI;
    case DISPLAY_OFFLINE_REASON_SETUP_TIMEOUT:
        return DISPLAY_FRAME_OFFLINE_SETUP_TIMEOUT;
    case DISPLAY_OFFLINE_REASON_API:
    default:
        return DISPLAY_FRAME_OFFLINE_API;
    }
}

static const char *offline_message_for_reason(display_offline_reason_t reason)
{
    switch (reason) {
    case DISPLAY_OFFLINE_REASON_WIFI:
        return "WiFi unreachable";
    case DISPLAY_OFFLINE_REASON_SETUP_TIMEOUT:
        return "Setup timed out";
    case DISPLAY_OFFLINE_REASON_API:
    default:
        return "API unreachable";
    }
}

typedef struct {
    const char *name;
    const char *reason;
} offline_row_t;

static void draw_offline_chrome(const char *label)
{
    hline(1,   1,   294);
    hline(1,   126, 294);
    vline(1,   1,   126);
    vline(294, 1,   126);

    icon_box_logo(6, 4);
    draw_str_adv(19, 4, "DEVDASH", 1, FONT_BOOT_W);
    int label_x = 290 - str_w(label);
    icon_cross_sync(label_x - 13, 4);
    draw_str(label_x, 4, label, 1);

    hline(2, 14, 293);
    hline(2, 114, 293);
}

static void draw_heading_with_dot(const char *left, const char *right)
{
    int x = 122;
    draw_str_adv(x, 22, left, 1, FONT_BOOT_W);
    x += str_w_adv(left, FONT_BOOT_W) + 6;
    fill_rect(x + 1, 25, 2, 2, 1, 0);
    x += 10;
    draw_str_adv(x, 22, right, 1, FONT_BOOT_W);
}

static void draw_footer_retry(const char *retry, uint32_t attempt)
{
    char attempt_text[32];
    if (attempt > 9999) {
        snprintf(attempt_text, sizeof(attempt_text), "attempt 9999+ | retry %s",
                 retry);
    } else {
        snprintf(attempt_text, sizeof(attempt_text), "attempt %lu | retry %s",
                 (unsigned long)attempt, retry);
    }
    draw_str_adv(6, 117, attempt_text, 1, FONT_BOOT_W);

    char setup[32];
    snprintf(setup, sizeof(setup), "hold BOOT %ds -> setup",
             CONFIG_DEVDASH_BOOT_LONGPRESS_MS / 1000);
    draw_str_adv(290 - str_w_adv(setup, FONT_BOOT_W), 117, setup, 1,
                 FONT_BOOT_W);
}

static void format_offline_retry(char *out, size_t out_sz,
                                 const dash_config_v2_t *cfg)
{
#if CONFIG_DEVDASH_RETRY_FOREVER_WHEN_OFFLINE
    snprintf(out, out_sz, "%ds", CONFIG_DEVDASH_OFFLINE_RETRY_INTERVAL_S);
#else
    uint8_t minutes = cfg && cfg->refresh_min ? cfg->refresh_min
                                              : CONFIG_DEVDASH_REFRESH_MIN;
    snprintf(out, out_sz, "%um", (unsigned)minutes);
#endif
}

static int enabled_network_count(const dash_config_v2_t *cfg)
{
    int count = 0;
    if (!cfg) return 0;
    for (uint8_t i = 0; i < cfg->network_count && i < MAX_WIFI_NETWORKS; i++) {
        const dash_wifi_profile_t *net = &cfg->networks[i];
        if (net->enabled && net->ssid[0] != '\0') count++;
    }
    return count;
}

static int collect_wifi_rows(const dash_config_v2_t *cfg,
                             const wifi_unreachable_diag_t *diag,
                             offline_row_t *rows,
                             int max_rows)
{
    int count = 0;
    if (!cfg || !diag) return 0;
    for (uint8_t i = 0; i < cfg->network_count &&
         i < MAX_WIFI_NETWORKS && count < max_rows; i++) {
        const dash_wifi_profile_t *net = &cfg->networks[i];
        if (!net->enabled || net->ssid[0] == '\0') continue;
        for (uint8_t j = 0; j < diag->row_count; j++) {
            if (diag->rows[j].network_idx != i) continue;
            rows[count++] = (offline_row_t){
                .name = net->ssid,
                .reason = diag->rows[j].reason,
            };
            break;
        }
    }
    return count;
}

static void copy_api_label(char *out, size_t out_sz, const char *url)
{
    if (out_sz == 0) return;
    out[0] = '\0';
    if (!url || !url[0]) return;

    const char *start = strstr(url, "://");
    start = start ? start + 3 : url;
    const char *end = start;
    while (*end && *end != '?' && *end != '#') end++;
    size_t len = (size_t)(end - start);
    while (len > 0 && start[len - 1] == '/') len--;
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, start, len);
    out[len] = '\0';
}

static int collect_api_rows(const dash_config_v2_t *cfg,
                            int network_idx,
                            const api_unreachable_diag_t *diag,
                            offline_row_t *rows,
                            char labels[][DASH_API_URL_MAX],
                            int max_rows)
{
    if (!cfg || !diag || network_idx < 0 || network_idx >= cfg->network_count) {
        return 0;
    }

    int count = 0;
    const dash_wifi_profile_t *net = &cfg->networks[network_idx];
    for (uint8_t i = 0; i < net->api_count &&
         i < MAX_APIS_PER_NETWORK && count < max_rows; i++) {
        const dash_api_profile_t *api = &net->apis[i];
        if (!api->enabled || api->api_url[0] == '\0') continue;
        const char *reason = NULL;
        for (uint8_t j = 0; j < diag->row_count; j++) {
            if (diag->rows[j].api_idx != i) continue;
            reason = diag->rows[j].reason;
            break;
        }
        if (!reason) continue;
        copy_api_label(labels[count], DASH_API_URL_MAX, api->api_url);
        rows[count++] = (offline_row_t){
            .name = labels[count - 1],
            .reason = reason,
        };
    }
    return count;
}

static void draw_offline_identity(const char *line2, const char *footnote)
{
    char footnote_fit[24];
    fit_text_ascii(footnote_fit, sizeof(footnote_fit), footnote, 92);

    fill_rect(6, 20, 108, 90, 1, 1);
    draw_str4x_inv(14, 30, "NO");
    draw_str4x_inv(14, 60, line2);
    draw_str_inv_adv(14, 98, footnote_fit, FONT_BOOT_W);
}

static void draw_fail_rows(const offline_row_t *rows, int row_count)
{
    for (int i = 0; i < row_count && i < 5; i++) {
        const int y = 41 + i * 10;
        char idx[4];
        snprintf(idx, sizeof(idx), "%d.", i + 1);
        draw_str_adv(130 - str_w_adv(idx, FONT_BOOT_W), y, idx, 1,
                     FONT_BOOT_W);

        const char *reason = rows[i].reason;
        if (!reason || reason[0] == '\0') continue;
        int reason_w = str_w(reason);
        draw_str(290 - reason_w, y, reason, 1);

        int name_left = 134;
        int name_w = 290 - reason_w - 4 - name_left;
        char name_fit[44];
        fit_text_ascii(name_fit, sizeof(name_fit), rows[i].name, name_w);
        draw_str_adv(name_left, y, name_fit, 1, FONT_BOOT_W);
    }
}

static void draw_unreachable_poster(display_offline_reason_t reason,
                                    const dash_config_v2_t *cfg,
                                    int network_idx,
                                    const wifi_unreachable_diag_t *wifi_diag,
                                    const api_unreachable_diag_t *api_diag,
                                    uint32_t attempt)
{
    offline_row_t rows[5] = {0};
    char api_labels[5][DASH_API_URL_MAX] = {{0}};
    char footnote[40];
    char retry[12];
    int row_count = 0;

    format_offline_retry(retry, sizeof(retry), cfg);

    if (reason == DISPLAY_OFFLINE_REASON_WIFI) {
        row_count = collect_wifi_rows(cfg, wifi_diag, rows, 5);
        snprintf(footnote, sizeof(footnote), "0/%d joined",
                 enabled_network_count(cfg));
        draw_offline_chrome("NO WIFI");
        draw_offline_identity("WIFI", footnote);
        draw_heading_with_dot("NETWORKS", "ALL FAILED");
    } else {
        if ((!cfg || network_idx < 0 || network_idx >= cfg->network_count) &&
            cfg && cfg->last_success_network_idx >= 0 &&
            cfg->last_success_network_idx < (int8_t)cfg->network_count) {
            network_idx = cfg->last_success_network_idx;
        }
        row_count = collect_api_rows(cfg, network_idx, api_diag, rows,
                                     api_labels, 5);
        const char *ssid = (cfg && network_idx >= 0 &&
                            network_idx < cfg->network_count)
            ? cfg->networks[network_idx].ssid : "wifi";
        snprintf(footnote, sizeof(footnote), "on %s", ssid);
        draw_offline_chrome("NO API");
        draw_offline_identity("API", footnote);
        draw_heading_with_dot("UPSTREAMS", "ALL DOWN");
    }

    hline(122, 35, 169);
    draw_fail_rows(rows, row_count);
    draw_footer_retry(retry, attempt);
}

static void ensure_init(void)
{
    validate_display_rtc_state();
    if (!s_initialized) {
        ESP_ERROR_CHECK(eink_init(&s_eink, effective_panel_variant()));
        s_initialized = true;
    }
}

static void warn_if_full_refresh_is_early(const char *reason)
{
#if CONFIG_DEVDASH_WARN_FULL_REFRESH_INTERVAL_S > 0
    if (s_last_full_refresh_us <= 0) return;

    int64_t now = esp_timer_get_time();
    int64_t min_us =
        (int64_t)CONFIG_DEVDASH_WARN_FULL_REFRESH_INTERVAL_S * 1000000LL;
    int64_t elapsed = now - s_last_full_refresh_us;
    if (elapsed >= min_us || elapsed < 0) return;

    ESP_LOGW(TAG,
             "%s full refresh only %lld s after previous full refresh",
             reason ? reason : "Display",
             (long long)(elapsed / 1000000LL));
#else
    (void)reason;
#endif
}

__attribute__((unused))
static void remember_current_bw_frame(void)
{
    memcpy(s_last_bw_buf, bw_buf, sizeof(s_last_bw_buf));
    s_last_bw_valid = true;
}

/* Wall-clock-trusted predicate: anything below 2024-01-01 UTC is treated
   as "SNTP has not corrected our RTC yet" — the firmware does not run
   SNTP today so this is dormant, but keeps the 24h forced-full path
   ready for the day it does. */
static bool wall_time_trusted(time_t *out_now)
{
    time_t now = time(NULL);
    if (now < (time_t)1704067200) return false;
    if (out_now) *out_now = now;
    return true;
}

static void commit_full_refresh_shared(bool need_red, bool wrote_safe_mode)
{
    /* Shared state after any successful full refresh (variant-aware or
       safe). Differences between the two profiles are layered on top by
       each caller below. */
    memcpy(s_last_bw_buf, bw_buf, sizeof(s_last_bw_buf));
    s_last_bw_valid = true;
    s_last_red_state =
        (effective_panel_variant() == EINK_PANEL_WEACT_29_BWR) ? need_red
                                                               : false;
    memset(s_region_partial_count, 0, sizeof(s_region_partial_count));
    s_offline_partial_count = 0;
    s_renders_since_full = 0;
    s_force_full_refresh = false;
    s_last_full_refresh_us = esp_timer_get_time();
    s_first_refresh_done = true;
    /* Variant-aware path persists last_var so the next render's
       variant-change detection compares against the last dashboard
       variant. Safe path skips last_var because a safe refresh does
       not represent the dashboard's variant. */
    if (!wrote_safe_mode) {
        disp_meta_set_last_var((uint8_t)effective_panel_variant());
    }
    /* Both paths count toward the 24h ghost-accumulation bound — a clean
       GC pass clears the panel regardless of which OTP waveform ran. */
    time_t now = 0;
    if (wall_time_trusted(&now)) {
        disp_meta_set_last_full((uint32_t)now);
    }
}

/* Variant-aware full refresh. Used by post-provisioning surfaces
   (dashboard, OTA, non-setup-timeout offline, status), all of which run
   with s_variant_known == true. */
static void display_full_refresh(bool need_red, const char *reason)
{
    warn_if_full_refresh_is_early(reason);
    eink_refresh_mode_t mode =
        (effective_panel_variant() == EINK_PANEL_WEACT_29_BW)
            ? EINK_REFRESH_BW_FULL
            : EINK_REFRESH_FULL_COLOR;
    eink_set_framebuffer(bw_buf, red_buf);
    eink_refresh(&s_eink, mode);
    commit_full_refresh_shared(need_red, /*wrote_safe_mode=*/false);
}

/* Panel-agnostic SAFE_BW full refresh. DORMANT since Gate 0.B: SAFE_BW could
   not clear pre-existing red on a red-preconditioned BWR panel, so recovery /
   provisioning surfaces now render through the variant-aware display_full_refresh()
   (FULL_COLOR clears red on BWR; BW_FULL on BW), with the panel variant resolved
   at boot from the saved config, the CONFIG_DEVDASH_DEFAULT_PANEL_VARIANT SKU
   default, or the captive portal panel selector persisted in NVS. This helper
   is retained as a build-stamped escape hatch but is no longer wired to any
   surface. */
__attribute__((unused))
static void display_full_refresh_safe(const char *reason)
{
    warn_if_full_refresh_is_early(reason);
    /* Pass NULL for red_buf: the caller supplies no red plane. The driver's
       SAFE_BW path seeds 0x26 itself (the BW base plane), so there is no
       caller red buffer to forward — NULL makes that intent explicit. */
    eink_set_framebuffer(bw_buf, NULL);
    eink_refresh(&s_eink, EINK_REFRESH_SAFE_BW);
    commit_full_refresh_shared(/*need_red=*/false, /*wrote_safe_mode=*/true);
}

static bool find_bw_diff_rect(const uint8_t *previous,
                              const uint8_t *next,
                              eink_rect_t *rect)
{
    int min_byte_x = EINK_WIDTH / 8;
    int max_byte_x = -1;
    int min_y = EINK_HEIGHT;
    int max_y = -1;
    const int row_bytes = EINK_WIDTH / 8;

    for (int y = 0; y < EINK_HEIGHT; y++) {
        const int row = y * row_bytes;
        for (int bx = 0; bx < row_bytes; bx++) {
            if (previous[row + bx] == next[row + bx]) {
                continue;
            }
            if (bx < min_byte_x) min_byte_x = bx;
            if (bx > max_byte_x) max_byte_x = bx;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }

    if (max_byte_x < 0) {
        return false;
    }

    rect->x = (uint16_t)(min_byte_x * 8);
    rect->y = (uint16_t)min_y;
    rect->w = (uint16_t)((max_byte_x - min_byte_x + 1) * 8);
    rect->h = (uint16_t)(max_y - min_y + 1);
    /* Narrow-X partial windows are validated on the WeAct 2.9 BW module
     * (BOARD_NOTES Gate 0.A — BW V2 partial LUT 0x32 + update trigger 0xCC).
     * The byte-aligned bounding box above is returned as-is; the former
     * full-row clamp is gone. This rect is still used only to answer
     * "did anything change?" — the per-region planner below owns the
     * actual partial windows. */
    return true;
}

/* Convert a logical (296x128 landscape) rect to a physical, byte-aligned
   framebuffer rect using the exact lpix() transform (px = ly,
   py = (EINK_HEIGHT-1) - lx). Logical X (lx/lw) maps to physical Y; logical
   Y (ly/lh) maps to physical X. Physical X is snapped down/up to 8-px byte
   bounds so the driver's alignment check passes and whole bytes are diffed. */
static eink_rect_t physical_rect_from_logical(int lx, int ly, int lw, int lh)
{
    int px_lo = ly;
    int px_hi = ly + lh - 1;
    int py_lo = EINK_HEIGHT - lx - lw;
    int py_hi = (EINK_HEIGHT - 1) - lx;

    px_lo &= ~7;            /* snap down to byte boundary */
    px_hi |= 7;             /* snap up   to byte boundary */

    if (px_lo < 0) px_lo = 0;
    if (px_hi > EINK_WIDTH - 1)  px_hi = EINK_WIDTH - 1;
    if (py_lo < 0) py_lo = 0;
    if (py_hi > EINK_HEIGHT - 1) py_hi = EINK_HEIGHT - 1;

    eink_rect_t r = {
        .x = (uint16_t)px_lo,
        .y = (uint16_t)py_lo,
        .w = (uint16_t)(px_hi - px_lo + 1),
        .h = (uint16_t)(py_hi - py_lo + 1),
    };
    return r;
}

/* True if any byte differs between a and b inside the physical rect r. */
static bool region_bytes_differ(const uint8_t *a, const uint8_t *b,
                                eink_rect_t r)
{
    const int row_bytes = EINK_WIDTH / 8;
    int x_byte  = r.x / 8;
    int x_bytes = r.w / 8;
    for (int row = r.y; row < r.y + r.h; row++) {
        int off = row * row_bytes + x_byte;
        if (memcmp(a + off, b + off, (size_t)x_bytes) != 0) return true;
    }
    return false;
}

/* True if any changed byte lies OUTSIDE every region rect (remainder check):
   the per-region plan is only valid when all changes are inside the declared
   regions; dividers / wide-layout / future widgets outside them force a full. */
static bool remainder_bytes_differ(const uint8_t *a, const uint8_t *b,
                                   const eink_rect_t *rects, int n)
{
    const int row_bytes = EINK_WIDTH / 8;
    for (int row = 0; row < EINK_HEIGHT; row++) {
        for (int bx = 0; bx < row_bytes; bx++) {
            int idx = row * row_bytes + bx;
            if (a[idx] == b[idx]) continue;
            bool covered = false;
            for (int i = 0; i < n && !covered; i++) {
                const eink_rect_t *r = &rects[i];
                int bx0 = r->x / 8;
                int bx1 = (r->x + r->w) / 8;   /* exclusive */
                if (row >= r->y && row < r->y + r->h &&
                    bx >= bx0 && bx < bx1) covered = true;
            }
            if (!covered) return true;
        }
    }
    return false;
}

/* Plan and execute a per-region BW partial refresh. Returns true only when
   every changed region updated successfully AND shared state was committed;
   false means the caller must fall back to a full BW refresh (which also
   re-syncs the panel with bw_buf). No shared state is mutated unless the whole
   plan succeeds — the commit-after-success invariant: a late failure must never
   leave the panel showing old regions while s_last_bw_buf already reflects the
   new frame. new_render_count is committed into s_renders_since_full on success.
   The full-refresh fallback path (display_full_refresh) is treated as
   infallible: eink_refresh() is void, so there is no failure signal to act on. */
static bool render_bw_regions(bool show_github, uint16_t new_render_count)
{
    const region_logical_t *regions = show_github ? s_regions_with_github
                                                   : s_regions_no_github;
    int n = show_github ? (int)ARRAY_SIZE(s_regions_with_github)
                        : (int)ARRAY_SIZE(s_regions_no_github);

    /* Diagnostic: show the configured cap and each region's accumulated partial
       count (pre-increment) so the cap behaviour is observable every cycle, not
       only when a region finally declines. A region forces a full once its count
       would exceed the cap. */
    char cnt[64];
    int coff = 0;
    for (int i = 0; i < n && coff < (int)sizeof(cnt) - 8; i++) {
        coff += snprintf(cnt + coff, sizeof(cnt) - coff, "%s%u",
                         i ? "," : "", (unsigned)s_region_partial_count[i]);
    }
    ESP_LOGI(TAG, "BW partial plan: cap=%u region_partials=[%s]",
             (unsigned)s_max_partials_per_region, cnt);

    eink_rect_t rects[REGION_COUNT_MAX];
    for (int i = 0; i < n; i++) {
        rects[i] = physical_rect_from_logical(regions[i].lx, regions[i].ly,
                                              regions[i].lw, regions[i].lh);
    }

    /* Any change outside the active region union -> caller does a full. */
    if (remainder_bytes_differ(s_last_bw_buf, bw_buf, rects, n)) {
        ESP_LOGI(TAG, "BW partial declined: change outside region union");
        return false;
    }

    /* Stage per-region counts without mutating any state yet. */
    uint8_t staged_count[REGION_COUNT_MAX];
    bool    changed[REGION_COUNT_MAX];
    int     changed_n = 0;
    for (int i = 0; i < n; i++) {
        changed[i]      = region_bytes_differ(s_last_bw_buf, bw_buf, rects[i]);
        staged_count[i] = s_region_partial_count[i];
        if (changed[i]) {
            staged_count[i] = (uint8_t)(s_region_partial_count[i] + 1);
            if (staged_count[i] > s_max_partials_per_region) {
                ESP_LOGI(TAG, "BW partial declined: region %d hit %d-partial cap",
                         i, s_max_partials_per_region);
                return false;
            }
            changed_n++;
        }
    }
    if (changed_n == 0) return false;   /* defensive: nothing to refresh */

    /* Execute. Any single failure aborts the plan; caller fulls + re-syncs. */
    for (int i = 0; i < n; i++) {
        if (!changed[i]) continue;
        if (!eink_refresh_bw_partial(&s_eink, s_last_bw_buf, bw_buf, rects[i])) {
            ESP_LOGW(TAG, "BW region %d partial failed; full re-sync", i);
            return false;
        }
    }

    /* Commit shared state only now that every partial succeeded. */
    memcpy(s_last_bw_buf, bw_buf, sizeof(s_last_bw_buf));
    s_last_bw_valid = true;
    for (int i = 0; i < n; i++) s_region_partial_count[i] = staged_count[i];
    s_renders_since_full          = new_render_count;
    s_last_dashboard_show_github  = show_github;
    s_last_dashboard_layout_valid = true;
    ESP_LOGI(TAG, "BW per-region partial: %d/%d regions updated", changed_n, n);
    return true;
}

/* ── public API ─────────────────────────────────────────────────────────── */

static bool dashboard_needs_physical_red(bool alert_requested)
{
    return effective_panel_variant() == EINK_PANEL_WEACT_29_BWR &&
           alert_requested;
}

/* Black bar across the bottom of the dashboard with white glyphs:
   [moon] SLEEPING [dot] WAKES HH:MM. Drawn last so it overlays the bottom row
   of dashboard content — intentional, the device is idle during quiet hours. */
static void draw_sleeping_footer(void)
{
    const int by = 113;                 /* bar top                          */
    const int bh = 12;                  /* bar height (to y=124, inside 126) */
    const int ty = by + 3;              /* glyph top                         */
    fill_rect(2, by, 291, bh, 1, 0);    /* black fill                        */

    int x = 7;
    icon_moon(x, by + 2, 0);            /* white crescent                    */
    x += 8 + 5;
    draw_str_inv_adv(x, ty, "SLEEPING", FONT_W);
    x += str_w("SLEEPING") + 6;
    fill_rect(x, ty + 2, 2, 2, 0, 0);   /* white floating dot                */
    x += 2 + 8;
    char wake[16];
    snprintf(wake, sizeof(wake), "WAKES %s",
             s_wake_hhmm[0] ? s_wake_hhmm : "--:--");
    draw_str_inv_adv(x, ty, wake, FONT_W);
}

static bool draw_dashboard_frame(const dashboard_data_t *data,
                                 const char *header_status,
                                 bool *alert_requested_out)
{
    memset(bw_buf,  0xFF, sizeof(bw_buf));   /* all white */
    memset(red_buf, 0x00, sizeof(red_buf));  /* no red    */

    bool offline    = data->offline;
    bool show_github = data->github_present;
    bool deps_alert = show_github && data->github.dependabot > 0;
    bool auth_err   = show_github && data->github.auth_error;
    bool github_err = show_github && (data->github.auth_error ||
                                      data->github.service_error);

    /* ── Outer border ── */
    hline(0,   0,   295);   /* top    */
    hline(0,   127, 295);   /* bottom */
    vline(0,   0,   127);   /* left   */
    vline(295, 0,   127);   /* right  */

    /* ── Header (y 2..14) ── */
    if (offline) {
        icon_cross_sync(6, 4);
        draw_str(19, 4, "OFFLINE", 1);
    } else {
        icon_box_logo(6, 4);
        draw_str(19, 4, "DEVDASH", 0);
        if ((!header_status || !header_status[0]) && !s_sleeping_mode) {
            draw_header_connection_slots();
        }
        if (header_status && header_status[0]) {
            draw_str(290 - str_w(header_status), 4, header_status, 0);
        } else if (s_sleeping_mode) {
            /* Sleeping: [moon] [HH:MM] of the last sync, no "+Nm" interval —
             * the device is parked, so the next-refresh hint is meaningless. */
            const int clock_w = str_w(data->updated_at);
            const int icon_gap = 4;
            const int moon_w   = 8;
            const int x_clock  = 290 - clock_w;
            const int x_moon   = x_clock - icon_gap - moon_w;
            icon_moon(x_moon, 4, 1);
            draw_str(x_clock, 4, data->updated_at, 0);
        } else {
            /* Right-anchored cluster: [sync] [HH:MM] [+Nm], all 5x7 font.
             * Wider intervals shift the clock/sync group left. */
            char next[8];
            snprintf(next, sizeof(next), "+%um", (unsigned)s_refresh_min);
            const int next_w  = str_w(next);
            const int clock_w = str_w(data->updated_at);
            const int text_gap = 2;
            const int icon_gap = 4;
            const int sync_w   = 8;
            const int x_next   = 290 - next_w;
            const int x_clock  = x_next - text_gap - clock_w;
            const int x_sync   = x_clock - icon_gap - sync_w;
            if (data->stale) {
                fill_rect(x_sync - 5, 7, 3, 3, 1, 0);
            }
            icon_sync(x_sync, 4);
            draw_str(x_clock, 4, data->updated_at, 0);
            draw_str(x_next,  4, next, 0);
        }
    }

    /* Header bottom hairline at y=14 */
    hline(1, 14, 293);

    provider_info_t active_providers[4];
    int num_providers = data->usage_count;
    if (num_providers > DASH_MAX_USAGE_SERVICES) {
        num_providers = DASH_MAX_USAGE_SERVICES;
    }
    for (int i = 0; i < num_providers; i++) {
        const usage_service_data_t *service = &data->usage[i];
        const usage_window_data_t *short_window = &service->windows[0];
        const usage_window_data_t *long_window = &service->windows[1];
        active_providers[i] = (provider_info_t){
            .label = service->label,
            .logo = provider_logo_from_name(service->icon),
            .ses_label = service->window_count > 0 ? short_window->label : "--",
            .wk_label = service->window_count > 1 ? long_window->label : "--",
            .ses = service->window_count > 0 ? short_window->used_pct : 0,
            .wk = service->window_count > 1 ? long_window->used_pct : 0,
            .ses_reset = service->window_count > 0
                ? short_window->reset_in_seconds : 0,
            .wk_reset = service->window_count > 1
                ? long_window->reset_in_seconds : 0,
            .extra = &service->extra_usage,
            .auth_err = service->service_error,
            .reached = (service->window_count > 0 && short_window->reached) ||
                       (service->window_count > 1 && long_window->reached),
        };
    }

    if (show_github) draw_github_strip(&data->github);

    int body_top = show_github ? 37 : 19;
    if (num_providers == 1) {
        draw_provider_hero(6, body_top, 288, &active_providers[0]);
    } else if (num_providers == 2) {
        int second_y = show_github ? 85 : 76;
        draw_provider_row(6, body_top, 288, &active_providers[0]);
        hline(6, show_github ? 80 : 71, 282);
        draw_provider_row(6, second_y, 288, &active_providers[1]);
    } else if (num_providers >= 3) {
        const int col_x[] = { 6, 156 };
        const int row_y[] = { body_top, show_github ? 86 : 77 };
        vline(148, show_github ? 35 : 17, show_github ? 90 : 108);
        hline(6, show_github ? 81 : 72, 282);
        for (int i = 0; i < num_providers && i < 4; i++) {
            draw_provider_grid(col_x[i % 2], row_y[i / 2], 138,
                               &active_providers[i]);
        }
    }

    bool alert_requested = deps_alert || auth_err || github_err || offline;
    for (int i = 0; i < num_providers; i++) {
        if (active_providers[i].ses > 80 || active_providers[i].wk > 80 ||
            active_providers[i].reached || active_providers[i].auth_err) {
            alert_requested = true;
        }
    }
    if (alert_requested_out) *alert_requested_out = alert_requested;

    if (s_sleeping_mode) draw_sleeping_footer();

    return dashboard_needs_physical_red(alert_requested);
}

void display_render(const dashboard_data_t *data)
{
    ensure_init();
    bool alert_requested = false;
    bool need_red = draw_dashboard_frame(data, NULL, &alert_requested);
    eink_rect_t diff_rect = {0};
    bool has_bw_diff = s_last_bw_valid &&
                       find_bw_diff_rect(s_last_bw_buf, bw_buf, &diff_rect);
    display_meta_t meta = {0};
    bool previous_frame_is_content =
        display_meta_load(&meta) && meta.frame == DISPLAY_FRAME_CONTENT;

    /* Variant-change check: a different variant has just taken effect
       (saved BWR → BW or vice versa via the portal restart path), so
       force a full refresh. The driver's variant field is already up to
       date through display_set_panel_variant() / ensure_init(). */
    uint8_t last_var = 0xFF;
    bool variant_changed = disp_meta_get_last_var(&last_var) &&
                           last_var != (uint8_t)effective_panel_variant();

    /* 24 h cycle-count fallback. Tick once per dashboard wake regardless
       of outcome (partial, full, or unchanged-skip) so an idle dashboard
       still gets a periodic full refresh to clear ghost accumulation. */
    uint16_t cap = render_count_cap(s_refresh_min);
    uint16_t staged_count = (uint16_t)(s_renders_since_full + 1u);
    bool cap_reached = staged_count >= cap;

    bool is_bw       = (effective_panel_variant() == EINK_PANEL_WEACT_29_BW);
    bool show_github = data->github_present;
    /* A show_github layout flip changes the active region set and indices and
       is NOT caught by the frame-type checks (both frames are CONTENT), so
       force a full BW refresh when the BW dashboard layout flips. */
    bool layout_flip = is_bw && s_last_dashboard_layout_valid &&
                       (s_last_dashboard_show_github != show_github);

    /* A sleeping-footer frame was the last thing pushed to the panel; the
       footer sits below the per-region partial rects, so only a full refresh
       reliably clears it. The flag is RTC-persisted across the quiet naps and
       consumed here on the first dashboard render after the window. */
    bool clear_sleeping = s_force_full_next_render;
    s_force_full_next_render = false;

    bool force_full = !s_first_refresh_done || s_force_full_refresh ||
                      variant_changed || cap_reached || layout_flip ||
                      clear_sleeping;

    ESP_LOGI(TAG,
             "Dashboard render decision: variant=%s alert=%d need_red=%d "
             "last_red=%d "
             "first=%d bw_valid=%d content_valid=%d frame_content=%d "
             "bw_diff=%d diff=(%u,%u %ux%u) renders=%u/%u force_full=%d "
             "variant_changed=%d",
             effective_panel_variant() == EINK_PANEL_WEACT_29_BW ? "BW" : "BWR",
             alert_requested, need_red, s_last_red_state, s_first_refresh_done,
             s_last_bw_valid, s_last_content_valid, previous_frame_is_content,
             has_bw_diff,
             diff_rect.x, diff_rect.y, diff_rect.w, diff_rect.h,
             (unsigned)staged_count, (unsigned)cap, force_full,
             variant_changed);

    bool did_partial = false;

    if (!force_full && s_last_bw_valid && !need_red && !s_last_red_state &&
        !has_bw_diff) {
        ESP_LOGI(TAG, "Dashboard unchanged; skipping refresh");
        eink_sleep(&s_eink);
        s_renders_since_full = staged_count;
    } else if (!force_full) {
        /* BW per-region partial path (validated Gate 0.A). Eligible only when
           the previously displayed frame was dashboard content (same layout
           family) and no red is involved; render_bw_regions enforces the
           remainder check and the per-region 5-partial cap, and commits shared
           state only on full success. Any decline/failure -> full BW refresh. */
        if (is_bw && !need_red && !s_last_red_state &&
            s_last_bw_valid && s_last_content_valid &&
            previous_frame_is_content) {
            did_partial = render_bw_regions(show_github, staged_count);
            if (did_partial) eink_sleep(&s_eink);
        }
        if (!did_partial) {
            display_full_refresh(need_red, "dashboard");
            eink_sleep(&s_eink);
            ESP_LOGI(TAG, "Dashboard refresh (mode=full, variant=%s)",
                     is_bw ? "BW_FULL" : "FULL_COLOR");
        }
    } else {
        const char *reason = variant_changed ? "variant-change"
                           : layout_flip     ? "layout-flip"
                           : cap_reached     ? "24h-cap"
                                             : "dashboard";
        display_full_refresh(need_red, reason);
        eink_sleep(&s_eink);
        ESP_LOGI(TAG, "Dashboard refresh forced full (reason=%s)", reason);
    }

    if (is_bw) {
        /* Keep the BW layout-flip guard in lockstep with what is now on the
           panel, on every outcome (unchanged-skip / partial / full). */
        s_last_dashboard_show_github  = show_github;
        s_last_dashboard_layout_valid = true;
    }

    if (!data->offline) {
        s_last_content_valid = true;
        memcpy(&s_last_data, data, sizeof(s_last_data));
        s_last_data_valid = true;
    }
    display_mark_frame(data->offline
                       ? DISPLAY_FRAME_OFFLINE_API
                       : DISPLAY_FRAME_CONTENT);
}

void display_show_sleeping(const char *wake_hhmm)
{
    ensure_init();
    snprintf(s_wake_hhmm, sizeof(s_wake_hhmm), "%s",
             (wake_hhmm && wake_hhmm[0]) ? wake_hhmm : "--:--");

    /* Compose the last shown dashboard plus the sleeping header/footer overlay
       and push it as one full refresh (once per window entry — main.c gates
       repeats with an RTC flag). Render from s_last_data so the footer sits on
       top of real content; on a cold session it falls back to a zeroed frame,
       which main.c avoids by only calling this after a prior successful
       render. */
    s_sleeping_mode = true;
    bool need_red = draw_dashboard_frame(&s_last_data, NULL, NULL);
    display_full_refresh(need_red, "sleeping");
    eink_sleep(&s_eink);
    s_sleeping_mode = false;

    /* Force the next real dashboard render to a full refresh so the footer is
       cleanly removed (it lies outside the per-region partial rects). */
    s_force_full_next_render = true;
}

void display_set_connection_slots(const dash_config_v2_t *cfg,
                                  int network_idx,
                                  int active_api_idx)
{
    memset(&s_header_slots, 0, sizeof(s_header_slots));
    if (!cfg || network_idx < 0 || network_idx >= cfg->network_count) return;

    const dash_wifi_profile_t *net = &cfg->networks[network_idx];
    s_header_slots.valid = true;
    s_header_slots.wifi_slot = (uint8_t)(network_idx + 1);
    if (active_api_idx >= 0 && active_api_idx < net->api_count) {
        s_header_slots.active_api_slot = (uint8_t)(active_api_idx + 1);
    }
}

static bool display_show_compact_status(const char *status)
{
    /* Hard precondition: compact status layers on top of an already-
       displayed dashboard frame, so the saved variant must match the
       physically attached panel. When ineligible (e.g. variant not yet
       known) this returns false and the caller draws the full-screen
       status poster through the variant-aware display_full_refresh() path. */
    if (!s_variant_known) {
        ESP_LOGW(TAG, "Compact status skipped: variant not yet known");
        return false;
    }
    if (!s_last_content_valid || !s_first_refresh_done ||
        !s_last_bw_valid || s_last_red_state) {
        return false;
    }

    display_meta_t meta = {0};
    if (!display_meta_load(&meta) || meta.frame != DISPLAY_FRAME_CONTENT) {
        return false;
    }

    ensure_init();
    memcpy(bw_buf, s_last_bw_buf, sizeof(bw_buf));
    memset(red_buf, 0x00, sizeof(red_buf));
    fill_rect(HEADER_STATUS_X, HEADER_STATUS_Y,
              HEADER_STATUS_W, HEADER_STATUS_H, 0, 0);
    hline(HEADER_STATUS_X, 1, HEADER_STATUS_W);
    hline(HEADER_STATUS_X, 15, HEADER_STATUS_W);
    draw_str(290 - str_w(status), 4, status, 0);
    eink_rect_t diff_rect = {0};
    bool has_bw_diff = find_bw_diff_rect(s_last_bw_buf, bw_buf, &diff_rect);
    if (!has_bw_diff) {
        eink_sleep(&s_eink);
        return true;
    }

    bool is_bw = (effective_panel_variant() == EINK_PANEL_WEACT_29_BW);
    bool did_partial = false;
    if (is_bw && s_last_dashboard_layout_valid) {
        /* The status overlay only touches the header region; reuse the
           per-region planner with the last dashboard layout. Do not tick the
           24 h counter (status is not a dashboard render) — commit the current
           s_renders_since_full unchanged. */
        did_partial = render_bw_regions(s_last_dashboard_show_github,
                                        s_renders_since_full);
        if (did_partial) eink_sleep(&s_eink);
    }
    if (!did_partial) {
        display_full_refresh(/*need_red=*/false, "status");
        eink_sleep(&s_eink);
    }
    ESP_LOGI(TAG, "Status refresh done");
    return true;
}

static void draw_boot_header(void)
{
    icon_box_logo(6, 4);
    draw_str_adv(19, 4, "DEVDASH", 1, FONT_BOOT_W);
    static const char *STATUS = "JOINING WIFI";
    draw_str_adv(291 - str_w_adv(STATUS, FONT_BOOT_W), 4, STATUS, 1,
                 FONT_BOOT_W);
    hline(2, 14, 293);
}

static void draw_boot_source_list(int lx, int ly)
{
    draw_str_adv(lx, ly, "github", 1, FONT_BOOT_W);
    lx += str_w_adv("github", FONT_BOOT_W) + FONT_BOOT_W;
    fill_rect(lx + 1, ly + 3, 2, 2, 1, 0);
    lx += 2 * FONT_BOOT_W;
    draw_str_adv(lx, ly, "ai usage", 1, FONT_BOOT_W);
}

static const char *display_firmware_version(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    if (app && app->version[0]) return app->version;
    return "unknown";
}

static void format_device_serial(char *out, size_t out_sz)
{
    uint8_t mac[6] = {0};
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(out, out_sz, "%02X-%02X-%02X", mac[3], mac[4], mac[5]);
        return;
    }
    snprintf(out, out_sz, "--");
}

static void draw_boot_footer(void)
{
    char fw[32];
    snprintf(fw, sizeof(fw), "fw %s", display_firmware_version());

    hline(2, 114, 293);
    draw_str_adv(6, 117, fw, 1, FONT_BOOT_W);

    static const char *SETUP = "hold BOOT 5s -> setup";
    draw_str_adv(291 - str_w_adv(SETUP, FONT_BOOT_W), 117, SETUP, 1,
                 FONT_BOOT_W);
}

static void draw_boot_networks(const dash_config_v2_t *cfg)
{
    int row = 0;
    for (uint8_t i = 0; cfg && i < cfg->network_count && row < 5; i++) {
        const dash_wifi_profile_t *net = &cfg->networks[i];
        if (!net->enabled || net->ssid[0] == '\0') continue;

        char index[3] = { (char)('1' + row), '.', '\0' };
        int y = 35 + row * 9;
        draw_str_adv(122, y, index, 1, FONT_BOOT_W);
        draw_str_clipped_adv(137, y, net->ssid, 1, FONT_BOOT_W,
                             290 - 137 + 1);
        row++;
    }
}

static void display_show_boot_poster(display_context_t ctx,
                                     const dash_config_v2_t *cfg)
{
    ensure_init();
    memset(bw_buf,  0xFF, sizeof(bw_buf));
    memset(red_buf, 0x00, sizeof(red_buf));

    hline(1,   1,   294);
    hline(1,   126, 294);
    vline(1,   1,   126);
    vline(294, 1,   126);

    draw_boot_header();

    fill_rect(6, 20, 108, 90, 1, 0);
    draw_str4x_bw(14, 30, "DEV", 0);
    draw_str4x_bw(14, 63, "DASH", 0);
    char serial[16];
    format_device_serial(serial, sizeof(serial));
    char sn[24];
    snprintf(sn, sizeof(sn), "sn %s", serial);
    draw_str_adv(14, 98, sn, 0, FONT_BOOT_W);

    draw_str_adv(122, 22, "SCANNING SAVED NETWORKS", 1, FONT_BOOT_W);
    draw_boot_networks(cfg);
    hline(122, 82, 169);
    draw_str_adv(122, 88, "THEN FETCH", 1, FONT_BOOT_W);
    draw_boot_source_list(122, 101);

    draw_boot_footer();

    /* Variant-aware in both contexts now (Gate 0.B): the panel variant is
       known at boot, so recovery uses FULL_COLOR on BWR (clears red) / BW_FULL
       on BW, not the dormant SAFE_BW path. */
    (void)ctx;
    display_full_refresh(/*need_red=*/false, "boot");
    eink_sleep(&s_eink);
    display_mark_frame(DISPLAY_FRAME_CONNECTING);
}

static void display_show_wait_page(display_context_t ctx,
                                   const char *header_status,
                                   const char *title,
                                   const char *sub)
{
    ensure_init();
    memset(bw_buf,  0xFF, sizeof(bw_buf));
    memset(red_buf, 0x00, sizeof(red_buf));

    hline(1,   1,   294);
    hline(1,   126, 294);
    vline(1,   1,   126);
    vline(294, 1,   126);

    icon_box_logo(6, 4);
    draw_str(19, 4, "DEVDASH", 0);
    draw_str(290 - str_w(header_status), 4, header_status, 0);
    hline(2, 15, 292);

    icon_wifi(24, 44);
    draw_str2x((296 - str2x_w(title)) / 2, 38, title, 0);
    draw_str((296 - str_w(sub)) / 2, 66, sub, 0);
    static const char *HINT = "Please wait";
    draw_str((296 - str_w(HINT)) / 2, 82, HINT, 0);

    /* Variant-aware in both contexts now (Gate 0.B); see display_show_boot_poster. */
    (void)ctx;
    display_full_refresh(/*need_red=*/false, "wait");
    eink_sleep(&s_eink);
    display_mark_frame(DISPLAY_FRAME_CONNECTING);
}

void display_show_connecting(display_context_t ctx, bool compact,
                             const dash_config_v2_t *cfg)
{
    /* Compact status overlays a saved dashboard frame, so it requires
       s_variant_known == true. The display_show_compact_status guard
       already enforces that and bails out otherwise; fall through to
       the variant-aware full poster below. */
    if (compact && ctx == DISPLAY_CTX_NORMAL_BOOT &&
        display_show_compact_status("connecting")) {
        return;
    }
    display_show_boot_poster(ctx, cfg);
}

void display_show_refreshing(display_context_t ctx, bool compact)
{
    if (compact && ctx == DISPLAY_CTX_NORMAL_BOOT &&
        display_show_compact_status("refreshing")) {
        return;
    }
    display_show_wait_page(ctx, "REFRESH", "Refreshing",
                           "Fetching dashboard data");
}

/* QR-render callback. Paints the QR matrix at the painted-area origin given
 * via user_data. The painted area is 89×89 starting at (12,20); we centre
 * the QR modules inside it and leave a 1-px white quiet zone. */
typedef struct {
    int origin_x;   /* left edge of the 89×89 painted area */
    int origin_y;   /* top  edge of the 89×89 painted area */
    int paint_sz;   /* always 89 in V4 S1 */
} qr_paint_ctx_t;

static void qr_paint_cb(esp_qrcode_handle_t qr, void *user_data)
{
    const qr_paint_ctx_t *ctx = (const qr_paint_ctx_t *)user_data;
    int n = esp_qrcode_get_size(qr);            /* modules per side */
    int module_px = 3;                          /* V4 spec */
    int modules_px = n * module_px;
    /* If the encoder picked a larger version than v3 (rare for our 40-char
     * WIFI string), shrink so we still fit. Quiet zone stays at 1 px. */
    while (modules_px > ctx->paint_sz - 2) {
        module_px--;
        if (module_px < 1) return;
        modules_px = n * module_px;
    }
    int offset_x = ctx->origin_x + (ctx->paint_sz - modules_px) / 2;
    int offset_y = ctx->origin_y + (ctx->paint_sz - modules_px) / 2;
    for (int my = 0; my < n; my++) {
        for (int mx = 0; mx < n; mx++) {
            if (!esp_qrcode_get_module(qr, mx, my)) continue;
            fill_rect(offset_x + mx * module_px,
                      offset_y + my * module_px,
                      module_px, module_px, 1, 0);
        }
    }
}

/* Shared header + footer + outer border for the V4 S1 setup screen. */
static void draw_s1_chrome(void)
{
    /* Outer 1-px border */
    hline(1,   1,   294);
    hline(1,   126, 294);
    vline(1,   1,   126);
    vline(294, 1,   126);

    /* Header (y 2..14) */
    icon_box_logo(6, 4);
    draw_str(19, 4, "DEVDASH", 0);
    static const char *SETUP = "SETUP";
    draw_str(290 - str_w(SETUP), 4, SETUP, 0);

    /* Header bottom hairline + footer top hairline */
    hline(2, 15,  292);
    hline(2, 113, 292);

    /* Footer caption — "Scan with phone camera", centred on y=117 */
    static const char *CAP = "Scan with phone camera";
    int cap_w = str_w(CAP);
    draw_str((296 - cap_w) / 2, 117, CAP, 0);
}

/* Render the V4 S1 info column at x≈113..292 with three labelled rows and
 * the two hint lines. Pass NULL/empty for values you want to blank out. */
static void draw_s1_info_column(const char *ssid,
                                const char *pwd,
                                const char *url,
                                bool show_hints)
{
    /* Row 1: SSID */
    icon_wifi(113, 22);
    draw_str(125, 26, "SSID", 0);
    draw_str(155, 26, ssid ? ssid : "—", 0);

    /* Row 2: PASS */
    icon_key(113, 44, 0);
    draw_str(125, 48, "PASS", 0);
    draw_str(155, 48, pwd ? pwd : "—", 0);

    /* Row 3: URL */
    icon_globe(113, 66);
    draw_str(125, 70, "URL", 0);
    draw_str(155, 70, url ? url : "—", 0);

    if (show_hints) {
        draw_str(115, 92,  "join & popup opens", 0);
        draw_str(115, 101, "- or visit URL.",    0);
    }
}

/* V4 S1 — pixel-exact e-ink provisioning prompt. ssid/pwd are the SoftAP
 * credentials; the QR encodes the standard WIFI:T:WPA;… join string. */
void display_show_qr(const char *ssid, const char *pop)
{
    ensure_init();
    memset(bw_buf,  0xFF, sizeof(bw_buf));
    memset(red_buf, 0x00, sizeof(red_buf));

    draw_s1_chrome();
    draw_s1_info_column(ssid ? ssid : "devdash-XXXX",
                        pop  ? pop  : "(see docs)",
                        "192.168.4.1",
                        true);

    /* QR painted area: 89×89 starting at (12,20). Already white from memset;
     * we draw black modules with a 1-px quiet zone via qr_paint_cb. */
    char qr_text[80];
    size_t qr_len = wifi_net_get_wifi_qr(qr_text, sizeof(qr_text));
    if (qr_len > 0) {
        qr_paint_ctx_t ctx = { .origin_x = 12, .origin_y = 20, .paint_sz = 89 };
        esp_qrcode_config_t cfg = {
            .display_func_with_cb = qr_paint_cb,
            .max_qrcode_version   = 4,           /* leaves headroom past v3-L */
            .qrcode_ecc_level     = ESP_QRCODE_ECC_LOW,
            .user_data            = &ctx,
        };
        esp_err_t err = esp_qrcode_generate(&cfg, qr_text);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "QR encode failed (%d); slot left blank", err);
        }
    }

    display_full_refresh(/*need_red=*/false, "provisioning");
    eink_sleep(&s_eink);
    display_mark_frame(DISPLAY_FRAME_QR);
}

/* V4 S1 error variant — drawn when the SoftAP failed to start. The QR slot
 * is replaced by a 19×19 red X plus two-line "SETUP / FAILED" red caption
 * and a black "check serial" hint. Info column gets em-dash placeholders. */
void display_show_setup_failed(void)
{
    ensure_init();
    memset(bw_buf,  0xFF, sizeof(bw_buf));
    memset(red_buf, 0x00, sizeof(red_buf));

    draw_s1_chrome();
    draw_s1_info_column(NULL, NULL, NULL, false);

    /* BigCross 19×19 centred horizontally in the 89×89 QR slot.
       Recovery surface: rendered through the variant-aware
       display_full_refresh() (FULL_COLOR on BWR, BW_FULL on BW). Drawn in
       black-only so the cross / SETUP-FAILED text renders identically on
       both panels. (Pre-Phase-1 BWR behavior rendered the cross in red;
       the variant-aware OTA / offline / dashboard alerts elsewhere still
       use red on BWR via display_full_refresh.) */
    for (int i = 0; i < 19; i++) {
        lpix(47 + i,        32 + i,        1, 0);
        if (i != 9) lpix(47 + i, 32 + 18 - i, 1, 0);
    }
    static const char *S1 = "SETUP";
    static const char *S2 = "FAILED";
    int qr_centre_x = 12 + 89 / 2;
    draw_str(qr_centre_x - str_w(S1) / 2, 60, S1, 0);
    draw_str(qr_centre_x - str_w(S2) / 2, 70, S2, 0);
    static const char *S3 = "check serial";
    draw_str(qr_centre_x - str_w(S3) / 2, 84, S3, 0);

    display_full_refresh(/*need_red=*/false, "setup failed");
    eink_sleep(&s_eink);
    display_mark_frame(DISPLAY_FRAME_QR);
}

/* ── Setup-mode reset flow (tap / hold) ──────────────────────────────────────
   A BOOT long-press in the portal brings up display_show_reset_confirm() —
   nothing is wiped yet, and the screen is drawn once (static, no countdown
   animation) so the gesture loop can poll BOOT continuously: a momentary tap is
   not lost to a blocking refresh. A short BOOT tap = config reset; holding BOOT
   ~3 s = full erase; no press = cancel. The result screens are pushed via a
   whole-panel partial refresh on BW (always, regardless of the max_partials
   cap) and a full refresh on BWR. */

/* Border + DEVDASH header with a right-aligned title; no QR footer. */
static void draw_reset_chrome(const char *title)
{
    hline(1,   1,   294);
    hline(1,   126, 294);
    vline(1,   1,   126);
    vline(294, 1,   126);

    icon_box_logo(6, 4);
    draw_str(19, 4, "DEVDASH", 0);
    draw_str(290 - str_w(title), 4, title, 0);

    hline(2, 15, 292);
}

/* Static confirm frame: TAP = config reset, HOLD = full erase, wait = cancel.
   Black-only so it renders identically on BW and BWR. */
static void draw_reset_confirm_frame(void)
{
    memset(bw_buf,  0xFF, sizeof(bw_buf));
    memset(red_buf, 0x00, sizeof(red_buf));

    draw_reset_chrome("SETUP RESET");

    draw_str2x(6, 24, "TAP = CONFIG RESET", 0);
    draw_str(6, 42, "clears wifi + api + sleep; panel kept", 0);

    draw_str2x(6, 64, "HOLD 3s = FULL ERASE", 0);
    draw_str(6, 82, "wipes entire nvs; back to first run", 0);

    hline(2, 100, 292);
    /* "15s" must match the cancel window in setup_reset_gesture() (wifi_prov.c). */
    draw_str(6, 106, "WAIT 15s = CANCEL", 0);
}

void display_show_reset_confirm(void)
{
    ensure_init();
    bool bwr = effective_panel_variant() == EINK_PANEL_WEACT_29_BWR;
    draw_reset_confirm_frame();
    /* Full refresh for a clean entry from the QR screen; commits the BW snapshot
       the result screen's whole-panel partial diffs against. Deliberately no
       eink_sleep(): the panel stays awake so the result push can partial-refresh. */
    display_full_refresh(/*need_red=*/bwr, "reset confirm");
    display_mark_frame(DISPLAY_FRAME_QR);
}

/* Push a freshly drawn (black-only) result frame. BW: whole-panel partial
   refresh — ALWAYS, regardless of settings — so there is no full-refresh flash;
   BWR (or BW with no valid snapshot): full refresh. Terminal: sleeps after. */
static void reset_result_commit(const char *reason)
{
    if (effective_panel_variant() == EINK_PANEL_WEACT_29_BW && s_last_bw_valid) {
        eink_rect_t full = physical_rect_from_logical(0, 0, 296, 128);
        if (eink_refresh_bw_partial(&s_eink, s_last_bw_buf, bw_buf, full)) {
            memcpy(s_last_bw_buf, bw_buf, sizeof(s_last_bw_buf));
            s_last_bw_valid = true;
            eink_sleep(&s_eink);
            display_mark_frame(DISPLAY_FRAME_QR);
            return;
        }
        ESP_LOGW(TAG, "reset result partial failed; full refresh");
    }
    display_full_refresh(/*need_red=*/false, reason);
    eink_sleep(&s_eink);
    display_mark_frame(DISPLAY_FRAME_QR);
}

/* Big centred banner (4×) over the standard chrome, with two sub-lines and a
   footer line. Used by the config-reset and full-erase result screens. */
static void draw_reset_result_banner(const char *title, const char *banner,
                                     const char *l1, const char *l2,
                                     const char *footer)
{
    memset(bw_buf,  0xFF, sizeof(bw_buf));
    memset(red_buf, 0x00, sizeof(red_buf));

    draw_reset_chrome(title);
    draw_str4x_bw((296 - (int)strlen(banner) * 23) / 2, 34, banner, 1);
    draw_str((296 - str_w(l1)) / 2, 74, l1, 0);
    draw_str((296 - str_w(l2)) / 2, 86, l2, 0);
    hline(2, 104, 292);
    draw_str((296 - str_w(footer)) / 2, 115, footer, 0);
}

void display_show_reset_result_config(void)
{
    ensure_init();
    draw_reset_result_banner("CONFIG RESET", "DONE",
                             "wifi + api + sleep cleared",
                             "panel + refresh settings kept",
                             "returning to setup...");
    reset_result_commit("reset done");
}

void display_show_reset_result_erase(void)
{
    ensure_init();
    draw_reset_result_banner("FULL ERASE", "WIPING",
                             "entire nvs erased on reboot",
                             "device restarts as first run",
                             "rebooting...");
    reset_result_commit("reset wiping");
}

void display_show_reset_result_fail(void)
{
    ensure_init();
    memset(bw_buf,  0xFF, sizeof(bw_buf));
    memset(red_buf, 0x00, sizeof(red_buf));

    draw_reset_chrome("SETUP RESET");
    draw_str2x((296 - str2x_w("NVS WRITE FAILED")) / 2, 42, "NVS WRITE FAILED", 0);
    draw_str((296 - str_w("config not cleared - nvs full?")) / 2, 66,
             "config not cleared - nvs full?", 0);
    hline(2, 104, 292);
    /* "10s" must match the fail retry/back window in setup_reset_gesture(). */
    draw_str((296 - str_w("BOOT = RETRY / WAIT 10s = BACK")) / 2, 115,
             "BOOT = RETRY / WAIT 10s = BACK", 0);

    reset_result_commit("reset fail");
}

void display_show_offline(display_offline_reason_t reason,
                          const dash_config_v2_t *cfg,
                          int network_idx,
                          const wifi_unreachable_diag_t *wifi_diag,
                          const api_unreachable_diag_t *api_diag,
                          uint32_t attempt)
{
    display_frame_t frame = offline_frame_for_reason(reason);
    if (reason == DISPLAY_OFFLINE_REASON_SETUP_TIMEOUT &&
        display_should_skip_offline_refresh(frame)) {
        ESP_LOGI(TAG, "Skipping offline refresh; offline frame already shown");
        return;
    }

    bool is_setup_timeout =
        (reason == DISPLAY_OFFLINE_REASON_SETUP_TIMEOUT);
    display_meta_t meta = {0};
    bool same_offline_frame =
        !is_setup_timeout &&
        display_meta_load(&meta) &&
        meta.frame == frame &&
        s_first_refresh_done &&
        s_last_bw_valid;

    if (is_setup_timeout) {
        ensure_init();
        memset(bw_buf,  0xFF, sizeof(bw_buf));
        memset(red_buf, 0x00, sizeof(red_buf));
        /* Provisioning recovery surface — rendered through the variant-aware
           display_full_refresh() (readable on either physical panel). Draw the
           alert in BW only (inlined cross with use_red=0) so it renders
           identically on both panels. icon_cross_sync() hardcodes use_red=1 and
           is used elsewhere by variant-aware paths where red is desired. */
        for (int i = 0; i < 8; i++) lpix(6 + i, 4 + i, 1, 0);
        static const int8_t d2[] = {0,7, 1,6, 2,5, 3,4, 5,3, 6,2, 7,1};
        for (int i = 0; i < 7; i++)
            lpix(6 + d2[i*2], 4 + d2[i*2+1], 1, 0);
        draw_str(19, 4, "OFFLINE", 0);
        hline(2, 15, 292);
        draw_str(6, 30, offline_message_for_reason(reason), 0);
        display_full_refresh(/*need_red=*/false, "offline-setup-timeout");
        eink_sleep(&s_eink);
        display_mark_frame(frame);
        return;
    }

    ensure_init();
    if (same_offline_frame) {
        /* Keep the NO WIFI or NO API diagnostic poster stable during an
         * outage. Only the footer attempt counter changes, which gives the BW
         * panel a small, fixed partial window and avoids redrawing diagnostic
         * rows every minute. */
        memcpy(bw_buf, s_last_bw_buf, sizeof(bw_buf));
        memset(red_buf, 0x00, sizeof(red_buf));
        fill_rect(2, 115, 291, 10, 0, 0);
        char retry[12];
        format_offline_retry(retry, sizeof(retry), cfg);
        draw_footer_retry(retry, attempt);

        eink_rect_t diff_rect = {0};
        if (!find_bw_diff_rect(s_last_bw_buf, bw_buf, &diff_rect)) {
            ESP_LOGI(TAG, "Offline frame unchanged; skipping refresh");
            eink_sleep(&s_eink);
            return;
        }

        if (effective_panel_variant() != EINK_PANEL_WEACT_29_BW) {
            ESP_LOGI(TAG,
                     "Offline attempt %lu recorded; BWR frame left unchanged "
                     "to avoid an unnecessary full refresh",
                     (unsigned long)attempt);
            eink_sleep(&s_eink);
            return;
        }

        uint16_t cap = render_count_cap(s_refresh_min);
        uint16_t staged_count =
            s_renders_since_full < UINT16_MAX
                ? (uint16_t)(s_renders_since_full + 1)
                : UINT16_MAX;
        if (offline_partial_refresh_allowed(
                s_offline_partial_count, s_max_partials_per_region,
                staged_count, cap)) {
            eink_rect_t footer =
                physical_rect_from_logical(0, 112, 296, 16);
            if (eink_refresh_bw_partial(&s_eink, s_last_bw_buf, bw_buf,
                                        footer)) {
                memcpy(s_last_bw_buf, bw_buf, sizeof(s_last_bw_buf));
                s_last_bw_valid = true;
                s_offline_partial_count++;
                s_renders_since_full = staged_count;
                eink_sleep(&s_eink);
                ESP_LOGI(TAG,
                         "Offline attempt %lu partial refresh (%u/%u)",
                         (unsigned long)attempt,
                         (unsigned)s_offline_partial_count,
                         (unsigned)s_max_partials_per_region);
                return;
            }
            ESP_LOGW(TAG, "Offline footer partial failed; full re-sync");
        } else {
            ESP_LOGI(TAG,
                     "Offline full refresh required: partials=%u/%u "
                     "renders=%u/%u",
                     (unsigned)s_offline_partial_count,
                     (unsigned)s_max_partials_per_region,
                     (unsigned)staged_count, (unsigned)cap);
        }
    } else {
        memset(bw_buf,  0xFF, sizeof(bw_buf));
        memset(red_buf, 0x00, sizeof(red_buf));
        draw_unreachable_poster(reason, cfg, network_idx, wifi_diag, api_diag,
                                attempt);
    }

    display_full_refresh(/*need_red=*/true, "offline");
    eink_sleep(&s_eink);
    display_mark_frame(frame);
}

static void draw_ota_header(void)
{
    icon_box_logo(6, 4);
    draw_str_adv(19, 4, "DEVDASH", 1, FONT_BOOT_W);

    static const char *label = "OTA UPDATE";
    int label_w = str_w_adv(label, FONT_BOOT_W);
    int label_x = 290 - label_w;
    icon_down_arrow(label_x - 11, 4);
    draw_str_adv(label_x, 4, label, 1, FONT_BOOT_W);
    hline(2, 14, 293);
}

static void copy_display_version(char *out, size_t out_sz, const char *version)
{
    if (out_sz == 0) return;
    if (!version || !version[0]) version = "unknown";
    if (version[0] == 'v' && version[1] != '\0') version++;
    strlcpy(out, version, out_sz);
}

static void draw_ota_identity(const char *from_version, const char *to_version)
{
    char from[16], to[16], pair[40], pair_fit[24];
    copy_display_version(from, sizeof(from), from_version);
    copy_display_version(to, sizeof(to), to_version);
    snprintf(pair, sizeof(pair), "fw %s > %s", from, to);
    fit_text_ascii(pair_fit, sizeof(pair_fit), pair, 100);

    fill_rect(6, 20, 108, 90, 1, 0);
    draw_str3x_inv(9, 24, "OTA");
    draw_str3x_inv(9, 54, "UPDATE");
    draw_str_inv_adv(9, 98, pair_fit, FONT_BOOT_W);
}

static void draw_ota_plan_row(int row, const char *name, const char *meta)
{
    const int y = 41 + row * 10;
    char idx[3] = { (char)('1' + row), '.', '\0' };
    draw_str_adv(130 - str_w_adv(idx, FONT_BOOT_W), y, idx, 1,
                 FONT_BOOT_W);

    int meta_w = str_w(meta);
    draw_str(290 - meta_w, y, meta, 0);

    int name_left = 134;
    int name_w = 290 - meta_w - 4 - name_left;
    char name_fit[32];
    fit_text_ascii(name_fit, sizeof(name_fit), name, name_w);
    draw_str_adv(name_left, y, name_fit, 1, FONT_BOOT_W);
}

static void draw_ota_footer_eta(void)
{
    static const char *ETA = "~40s";
    static const char *REBOOT = "reboot once";
    int eta_w = str_w_adv(ETA, FONT_BOOT_W);
    int reboot_w = str_w_adv(REBOOT, FONT_BOOT_W);
    int lx = 290 - (eta_w + 3 * FONT_BOOT_W + reboot_w);

    draw_str_adv(lx, 117, ETA, 1, FONT_BOOT_W);
    lx += eta_w + FONT_BOOT_W;
    fill_rect(lx + 1, 120, 2, 2, 1, 0);
    lx += 2 * FONT_BOOT_W;
    draw_str_adv(lx, 117, REBOOT, 1, FONT_BOOT_W);
}

void display_show_ota_update(const char *from_version,
                             const char *to_version,
                             const char *slot_name,
                             const char *slot_label)
{
    ensure_init();
    memset(bw_buf,  0xFF, sizeof(bw_buf));
    memset(red_buf, 0x00, sizeof(red_buf));

    hline(1,   1,   294);
    hline(1,   126, 294);
    vline(1,   1,   126);
    vline(294, 1,   126);

    draw_ota_header();
    draw_ota_identity(from_version, to_version);

    draw_str_adv(122, 22, "INSTALLING UPDATE", 1, FONT_BOOT_W);
    hline(122, 35, 169);
    draw_ota_plan_row(0, "download", "GitHub");
    draw_ota_plan_row(1, "verify app", "image hdr");

    char flash_meta[24], boot_meta[20], flash_name[16];
    snprintf(flash_name, sizeof(flash_name), "flash %s",
             slot_name && slot_name[0] ? slot_name : "ota");
    snprintf(flash_meta, sizeof(flash_meta), "partition %s",
             slot_label && slot_label[0] ? slot_label : "?");
    snprintf(boot_meta, sizeof(boot_meta), "boot %s",
             slot_label && slot_label[0] ? slot_label : "?");
    draw_ota_plan_row(2, flash_name, flash_meta);
    draw_ota_plan_row(3, "reboot", boot_meta);

    hline(2, 114, 293);
    fill_rect(6, 119, 3, 3, 1, 1);
    draw_str(13, 117, "DO NOT UNPLUG", 1);
    draw_ota_footer_eta();

    display_full_refresh(/*need_red=*/true, "OTA update");
    eink_sleep(&s_eink);
    display_mark_frame(DISPLAY_FRAME_OTA);
}
