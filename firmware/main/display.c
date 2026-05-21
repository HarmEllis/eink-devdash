/*
 * V3 Dashboard — landscape layout (logical 296×128 px).
 *
 * Physical buffer: 128 wide × 296 tall (portrait, SSD1680).
 * Coordinate rotation 90° CW: logical (lx, ly) → physical (127−ly, lx).
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
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "nvs.h"
#include "qrcode.h"
#include "wifi_prov.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "display";

/* ── 5×7 pixel font (ASCII 32–122) ─────────────────────────────────────── */
#define FONT_W  6   /* 5 px glyph + 1 px gap */
#define FONT_H  7
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
};

/* ── framebuffers ───────────────────────────────────────────────────────── */
static uint8_t bw_buf[EINK_BUF_SIZE];
static uint8_t red_buf[EINK_BUF_SIZE];

/* ── landscape pixel primitive ──────────────────────────────────────────── */
/* lx ∈ [0,295], ly ∈ [0,127].  Rotation 90° CW → physical px=127−ly, py=lx */
static void lpix(int lx, int ly, int black, int use_red)
{
    int px = ly;
    int py = (EINK_HEIGHT - 1) - lx;
    if ((unsigned)px >= EINK_WIDTH || (unsigned)py >= EINK_HEIGHT) return;
    int i = (py * EINK_WIDTH + px) / 8;
    int b = 7 - ((py * EINK_WIDTH + px) % 8);
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
    if ((unsigned char)c < 32 || (unsigned char)c > 122) c = '?';
    const uint8_t *g = font5x7[(unsigned char)c - 32];
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 7; row++)
            if (g[col] & (1 << row))
                lpix(lx + col, ly + row, 1, use_red);
}

static void draw_str(int lx, int ly, const char *s, int use_red)
{
    while (*s) { draw_char(lx, ly, *s++, use_red); lx += FONT_W; }
}

static int str_w(const char *s)  { return (int)strlen(s) * FONT_W; }

/* 2× scaled */
static void draw_char2x(int lx, int ly, char c, int use_red)
{
    if ((unsigned char)c < 32 || (unsigned char)c > 122) c = '?';
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

/* ── pixel icons ────────────────────────────────────────────────────────── */

static void icon_box_logo(int ox, int oy)
{
    fill_rect(ox,   oy,   9, 9, 1, 0);  /* 9×9 solid black  */
    fill_rect(ox+2, oy+2, 5, 5, 0, 0);  /* 5×5 white cutout */
    fill_rect(ox+3, oy+3, 3, 3, 1, 0);  /* 3×3 black centre */
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
static void draw_bar_cfg(int ox, int oy, int width, int height, int seg_w, int pct)
{
    int stride = seg_w + 1;
    int cols = (width + 1) / stride;
    int filled = (pct * cols + 50) / 100;
    int thresh = (80 * cols + 50) / 100;
    for (int i = 0; i < cols; i++) {
        int sx = ox + i * stride;
        if (i < filled) {
            int is_red = (pct > 80) && (i >= thresh);
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

typedef struct {
    int title_row_h;
    int row_gap;
    int bar_row_h;
    int bar_h;
    int seg_w;
    int row_label_w;
    int pct_w;
} provider_layout_t;

/* ── provider bar block ─────────────────────────────────────────────────── */
/* V3 updated layout. Compact mode sits next to the GitHub column; wide mode
 * spans the full panel when the API response has no GitHub object. */
static void draw_provider(int ox, int oy, int width, const provider_layout_t *layout,
                          const char *title, int ses, int wk,
                          int ses_reset_s, int wk_reset_s)
{
    char pct_s[8], ses_s[8], wk_s[8];

    draw_str(ox, oy, title, 0);

    format_reset_countdown(ses_s, sizeof(ses_s), ses_reset_s);
    format_reset_countdown(wk_s,  sizeof(wk_s),  wk_reset_s);

    const int text_gap = 2;
    const int hg_gap   = 4;
    const int hg_w     = 7;
    int right = ox + width - 2;
    int x_wk    = right - str_w(wk_s);
    int x_slash = x_wk - text_gap - str_w("/");
    int x_ses   = x_slash - text_gap - str_w(ses_s);
    int x_hg    = x_ses - hg_gap - hg_w;
    icon_hourglass(x_hg, oy, 0);
    draw_str(x_ses,   oy, ses_s, ses > 80);
    draw_str(x_slash, oy, "/",   0);
    draw_str(x_wk,    oy, wk_s,  wk  > 80);

    int bar_x = ox + layout->row_label_w;
    int bar_w = width - layout->row_label_w - layout->pct_w;
    int ses_y = oy + layout->title_row_h + layout->row_gap;
    int wk_y  = ses_y + layout->bar_row_h + 2;
    int bar_dy = (layout->bar_row_h - layout->bar_h) / 2;

    draw_str(ox, ses_y + 2, "5H", 0);
    draw_bar_cfg(bar_x, ses_y + bar_dy, bar_w, layout->bar_h, layout->seg_w, ses);
    snprintf(pct_s, sizeof(pct_s), "%d%%", ses);
    draw_str(ox + width - str_w(pct_s) - 2, ses_y + 2, pct_s, ses > 80);

    draw_str(ox, wk_y + 2, "WK", 0);
    draw_bar_cfg(bar_x, wk_y + bar_dy, bar_w, layout->bar_h, layout->seg_w, wk);
    snprintf(pct_s, sizeof(pct_s), "%d%%", wk);
    draw_str(ox + width - str_w(pct_s) - 2, wk_y + 2, pct_s, wk > 80);
}

/* ── icon row (left column) ─────────────────────────────────────────────── */
/* Each row is 16 px tall; icon centered at y+3, label at y+4, value at y+1 */
static void draw_icon_row(int row_y,
                          void (*icon_fn)(int, int, int), int icon_red,
                          const char *label,
                          const char *value, int value_red)
{
    icon_fn(6, row_y + 3, icon_red);
    draw_str(22, row_y + 4, label, 0);
    /* 2× value, right-aligned inside the 96 px GitHub column. */
    draw_str2x(102 - str2x_w(value), row_y + 1, value, value_red);
}

/* ── device handle ──────────────────────────────────────────────────────── */
static eink_handle_t s_eink;
static bool          s_initialized        = false;

/* Refresh-cycle state lives in RTC slow memory: survives deep-sleep timer
 * wakeups (so the BW_FAST differential refresh stays valid across naps) but
 * resets on power-on / external reset (where the panel content is undefined
 * anyway and we want a FULL_COLOR refresh). */
static RTC_DATA_ATTR bool    s_first_refresh_done = false;
static RTC_DATA_ATTR uint8_t s_bw_fast_cycle_count = 0;
static RTC_DATA_ATTR bool    s_last_red_state      = false;
static RTC_DATA_ATTR bool    s_last_content_valid  = false;

#define DISPLAY_META_NAMESPACE "disp_meta"
#define DISPLAY_META_KEY       "state"
#define DISPLAY_META_MAGIC     0xD15DA5E1u

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

static void ensure_init(void)
{
    if (!s_initialized) {
        ESP_ERROR_CHECK(eink_init(&s_eink));
        s_initialized = true;
    }
}

static void refresh_logical_bw_area(int lx, int ly, int w, int h)
{
    int phys_x = ly;
    int phys_y = (EINK_HEIGHT - 1) - (lx + w - 1);
    int phys_w = h;
    int phys_h = w;

    eink_set_framebuffer(bw_buf, NULL);
    eink_refresh_bw_area(&s_eink, phys_x, phys_y, phys_w, phys_h);
}

/* ── public API ─────────────────────────────────────────────────────────── */

static bool draw_dashboard_frame(const dashboard_data_t *data,
                                 const char *header_status)
{
    memset(bw_buf,  0xFF, sizeof(bw_buf));   /* all white */
    memset(red_buf, 0x00, sizeof(red_buf));  /* no red    */

    /* Derive percentages from raw counts */
    int claude_ses = (data->claude.five_hour.limit > 0)
        ? data->claude.five_hour.used * 100 / data->claude.five_hour.limit : 0;
    int claude_wk  = (data->claude.weekly.limit > 0)
        ? data->claude.weekly.used  * 100 / data->claude.weekly.limit  : 0;
    int codex_ses  = data->codex.short_pct;
    int codex_wk   = data->codex.long_pct;

    bool offline    = data->offline || data->stale;
    bool show_github = data->github_present;
    bool deps_alert = show_github && data->github.dependabot > 0;
    bool auth_err   = show_github && data->github.auth_error;

    /* ── Outer border ── */
    hline(1,   1,   294);   /* top    */
    hline(1,   126, 294);   /* bottom */
    vline(1,   1,   126);   /* left   */
    vline(294, 1,   126);   /* right  */

    /* ── Header (y 2..14) ── */
    if (offline) {
        icon_cross_sync(6, 4);
        draw_str(19, 5, "OFFLINE", 1);
    } else {
        icon_box_logo(6, 4);
        draw_str(19, 5, "DEVDASH", 0);
        if (header_status && header_status[0]) {
            draw_str(290 - str_w(header_status), 5, header_status, 0);
        } else {
            /* Right-anchored cluster: [sync] [HH:MM] [+5m], all 5×7 font.
             * Coordinates pinned: sync 228..235, clock 240..269, +5m 272..289. */
            static const char *NEXT = "+5m";
            const int next_w  = str_w(NEXT);
            const int clock_w = str_w(data->updated_at);
            const int text_gap = 2;
            const int icon_gap = 4;
            const int sync_w   = 8;
            const int x_next   = 290 - next_w;
            const int x_clock  = x_next - text_gap - clock_w;
            const int x_sync   = x_clock - icon_gap - sync_w;
            icon_sync(x_sync, 4);
            draw_str(x_clock, 5, data->updated_at, 0);
            draw_str(x_next,  5, NEXT, 0);
        }
    }

    /* Header bottom hairline at y=15 */
    hline(2, 15, 292);

    static const provider_layout_t compact_provider = {
        .title_row_h = 14,
        .row_gap = 2,
        .bar_row_h = 12,
        .bar_h = 10,
        .seg_w = 3,
        .row_label_w = 18,
        .pct_w = 28,
    };
    static const provider_layout_t wide_provider = {
        .title_row_h = 18,
        .row_gap = 3,
        .bar_row_h = 14,
        .bar_h = 12,
        .seg_w = 4,
        .row_label_w = 22,
        .pct_w = 36,
    };

    if (show_github) {
        /* Column divider at x=106. The narrower GitHub column leaves more
         * room for provider bars and their reset countdowns. */
        vline(106, 16, 110);

        /* ── Left column — icon rows ── */
        char v[8];

        /* Row 1: ISSUES @ y=20 */
        snprintf(v, sizeof(v), "%d", data->github.issues);
        draw_icon_row(20, icon_issue, 0, "ISSUES", v, 0);

        /* Row 2: PRs @ y=42 */
        snprintf(v, sizeof(v), "%d", data->github.prs);
        draw_icon_row(42, icon_pr, 0, "PRs", v, 0);

        /* Row 3: DEPS @ y=64 */
        if (deps_alert)
            snprintf(v, sizeof(v), "%d!", data->github.dependabot);
        else
            snprintf(v, sizeof(v), "%d", data->github.dependabot);
        draw_icon_row(64, icon_shield, deps_alert, "DEPS", v, deps_alert);

        /* Row 4: AUTH @ y=86 */
        draw_icon_row(86, icon_key, auth_err, "AUTH",
                      auth_err ? "ERR" : "OK", auth_err);

        /* ── Right column — compact provider bars ── */
        draw_provider(112, 20, 182, &compact_provider, "CLAUDE",
                      claude_ses, claude_wk,
                      data->claude.five_hour.reset_in_seconds,
                      data->claude.weekly.reset_in_seconds);
        draw_provider(112, 66, 182, &compact_provider, "CODEX",
                      codex_ses, codex_wk,
                      data->codex.short_reset_in_seconds,
                      data->codex.long_reset_in_seconds);
    } else {
        /* GitHub integration disabled upstream: use the whole canvas for the
         * provider panels instead of rendering an empty GitHub side. */
        draw_provider(4, 20, 288, &wide_provider, "CLAUDE",
                      claude_ses, claude_wk,
                      data->claude.five_hour.reset_in_seconds,
                      data->claude.weekly.reset_in_seconds);
        draw_provider(4, 76, 288, &wide_provider, "CODEX",
                      codex_ses, codex_wk,
                      data->codex.short_reset_in_seconds,
                      data->codex.long_reset_in_seconds);
    }

    return deps_alert || auth_err || offline
           || claude_ses > 80 || claude_wk > 80
           || codex_ses > 80  || codex_wk  > 80
           || data->codex.reached;
}

void display_render(const dashboard_data_t *data)
{
    ensure_init();
    bool need_red = draw_dashboard_frame(data, NULL);

    /* ── Refresh mode ──
     * FULL_COLOR is forced on three conditions:
     *   1. need_red          — this frame paints red
     *   2. !first_refresh    — cold boot, BW_FAST diff is undefined
     *   3. s_last_red_state  — previous frame had red; BW_FAST can't clear it
     * Otherwise use BW_FAST, with a periodic FULL_COLOR every 10 cycles to
     * heal accumulated drift. s_last_red_state is tracked globally across all
     * display_* entry points so the red plane stays consistent. */
    eink_refresh_mode_t mode;
    if (need_red || !s_first_refresh_done || s_last_red_state) {
        mode = EINK_REFRESH_FULL_COLOR;
        s_bw_fast_cycle_count = 0;
    } else {
        s_bw_fast_cycle_count++;
        if (s_bw_fast_cycle_count >= 10) {
            mode = EINK_REFRESH_FULL_COLOR;
            s_bw_fast_cycle_count = 0;
        } else {
            mode = EINK_REFRESH_BW_FAST;
        }
    }
    s_last_red_state     = need_red;
    s_first_refresh_done = true;

    eink_set_framebuffer(bw_buf, red_buf);
    eink_refresh(&s_eink, mode);
    eink_sleep(&s_eink);
    if (!data->offline && !data->stale) {
        s_last_content_valid = true;
    }
    display_mark_frame((data->offline || data->stale)
                       ? DISPLAY_FRAME_OFFLINE_API
                       : DISPLAY_FRAME_CONTENT);
}

static bool display_show_compact_status(const char *status)
{
    if (!s_last_content_valid || !s_first_refresh_done) {
        return false;
    }

    display_meta_t meta = {0};
    if (!display_meta_load(&meta) || meta.frame != DISPLAY_FRAME_CONTENT) {
        return false;
    }

    ensure_init();
    memset(bw_buf,  0xFF, sizeof(bw_buf));
    memset(red_buf, 0x00, sizeof(red_buf));
    fill_rect(HEADER_STATUS_X, HEADER_STATUS_Y,
              HEADER_STATUS_W, HEADER_STATUS_H, 0, 0);
    hline(HEADER_STATUS_X, 1, HEADER_STATUS_W);
    hline(HEADER_STATUS_X, 15, HEADER_STATUS_W);
    draw_str(290 - str_w(status), 5, status, 0);
    refresh_logical_bw_area(HEADER_STATUS_X, HEADER_STATUS_Y,
                            HEADER_STATUS_W, HEADER_STATUS_H);
    eink_sleep(&s_eink);
    s_first_refresh_done = true;
    return true;
}

static void display_show_wait_page(const char *header_status,
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
    draw_str(19, 5, "DEVDASH", 0);
    draw_str(290 - str_w(header_status), 5, header_status, 0);
    hline(2, 15, 292);

    icon_wifi(24, 44);
    draw_str2x((296 - str2x_w(title)) / 2, 38, title, 0);
    draw_str((296 - str_w(sub)) / 2, 66, sub, 0);
    static const char *HINT = "Please wait";
    draw_str((296 - str_w(HINT)) / 2, 82, HINT, 0);

    eink_refresh_mode_t mode =
        (!s_first_refresh_done || s_last_red_state)
            ? EINK_REFRESH_FULL_COLOR
            : EINK_REFRESH_BW_FAST;
    eink_set_framebuffer(bw_buf, red_buf);
    eink_refresh(&s_eink, mode);
    eink_sleep(&s_eink);
    s_first_refresh_done = true;
    s_last_red_state     = false;
    display_mark_frame(DISPLAY_FRAME_CONNECTING);
}

void display_show_connecting(bool compact)
{
    if (compact && display_show_compact_status("connecting")) {
        return;
    }
    display_show_wait_page("CONNECT", "Joining WiFi", "Scanning saved networks");
}

void display_show_refreshing(bool compact)
{
    if (compact && display_show_compact_status("refreshing")) {
        return;
    }
    display_show_wait_page("REFRESH", "Refreshing", "Fetching dashboard data");
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
    draw_str(19, 5, "DEVDASH", 0);
    static const char *SETUP = "SETUP";
    draw_str(290 - str_w(SETUP), 5, SETUP, 0);

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

    eink_set_framebuffer(bw_buf, red_buf);
    eink_refresh(&s_eink, EINK_REFRESH_FULL_COLOR);
    eink_sleep(&s_eink);
    s_first_refresh_done = true;
    s_last_red_state     = false;   /* S1 prompt paints no red */
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

    /* BigCross 19×19 centred horizontally in the 89×89 QR slot. The slot
     * starts at x=12 and is 89 wide; (89-19)/2 = 35 → cross origin x = 47. */
    icon_big_cross_red(47, 32);
    /* SETUP / FAILED two-line red, centred under the cross. */
    static const char *S1 = "SETUP";
    static const char *S2 = "FAILED";
    int qr_centre_x = 12 + 89 / 2;
    draw_str(qr_centre_x - str_w(S1) / 2, 60, S1, 1);
    draw_str(qr_centre_x - str_w(S2) / 2, 70, S2, 1);
    /* Black "check serial" hint below. */
    static const char *S3 = "check serial";
    draw_str(qr_centre_x - str_w(S3) / 2, 84, S3, 0);

    eink_set_framebuffer(bw_buf, red_buf);
    eink_refresh(&s_eink, EINK_REFRESH_FULL_COLOR);
    eink_sleep(&s_eink);
    s_first_refresh_done = true;
    s_last_red_state     = true;    /* big-cross + SETUP/FAILED are red */
    display_mark_frame(DISPLAY_FRAME_QR);
}

void display_show_offline(display_offline_reason_t reason)
{
    display_frame_t frame = offline_frame_for_reason(reason);
    if (display_should_skip_offline_refresh(frame)) {
        ESP_LOGI(TAG, "Skipping offline refresh; offline frame already shown");
        return;
    }

    ensure_init();
    memset(bw_buf,  0xFF, sizeof(bw_buf));
    memset(red_buf, 0x00, sizeof(red_buf));
    icon_cross_sync(6, 4);
    draw_str(19, 5, "OFFLINE", 1);
    hline(2, 15, 292);
    draw_str(6, 30, offline_message_for_reason(reason), 0);
    eink_set_framebuffer(bw_buf, red_buf);
    /* The offline frame paints red; BW_FAST cannot write the red plane, so
     * always use FULL_COLOR here. */
    eink_refresh(&s_eink, EINK_REFRESH_FULL_COLOR);
    eink_sleep(&s_eink);
    s_first_refresh_done = true;
    s_last_red_state     = true;
    display_mark_frame(frame);
}
