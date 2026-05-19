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

/* ── segmented bar ──────────────────────────────────────────────────────── */
/* 30 segments × 3 px wide × 7 px tall, 1 px gap.
   Segments past column 24 (=80%) render RED when pct > 80.            */
static void draw_bar(int ox, int oy, int pct)
{
    int filled   = (pct * 30 + 50) / 100;
    const int thresh = 24;                   /* round(80/100 * 30) */
    for (int i = 0; i < 30; i++) {
        int sx = ox + i * 4;
        if (i < filled) {
            int is_red = (pct > 80) && (i >= thresh);
            fill_rect(sx, oy, 3, 7, 1, is_red);
        } else {
            /* outlined empty box: 1 px black border, white interior */
            hline(sx, oy,   3);
            hline(sx, oy+6, 3);
            vline(sx,   oy+1, 5);
            vline(sx+2, oy+1, 5);
        }
    }
}

/* ── provider bar block ─────────────────────────────────────────────────── */
static void draw_provider(int ox, int oy, const char *title, int ses, int wk)
{
    char pct_s[8];

    draw_str(ox, oy, title, 0);

    draw_str(ox, oy + 11, "SES", 0);
    draw_bar(ox + 18, oy + 11, ses);
    snprintf(pct_s, sizeof(pct_s), "%d%%", ses);
    draw_str(290 - str_w(pct_s), oy + 11, pct_s, ses > 80);

    draw_str(ox, oy + 22, "WK", 0);
    draw_bar(ox + 18, oy + 22, wk);
    snprintf(pct_s, sizeof(pct_s), "%d%%", wk);
    draw_str(290 - str_w(pct_s), oy + 22, pct_s, wk > 80);
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
    /* 2× value, right-aligned, ends at x=112 (col-right 116 minus 4 px pad) */
    draw_str2x(112 - str2x_w(value), row_y + 1, value, value_red);
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

#define DISPLAY_META_NAMESPACE "disp_meta"
#define DISPLAY_META_KEY       "state"
#define DISPLAY_META_MAGIC     0xD15DA5E1u

typedef enum {
    DISPLAY_FRAME_UNKNOWN = 0,
    DISPLAY_FRAME_CONTENT = 1,
    DISPLAY_FRAME_OFFLINE = 2,
    DISPLAY_FRAME_QR      = 3,
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

static bool display_should_skip_offline_refresh(void)
{
    esp_reset_reason_t reset = esp_reset_reason();
    if (reset == ESP_RST_POWERON || reset == ESP_RST_BROWNOUT) {
        return false;
    }

    display_meta_t meta = {0};
    return display_meta_load(&meta) && meta.frame == DISPLAY_FRAME_OFFLINE;
}

static void ensure_init(void)
{
    if (!s_initialized) {
        ESP_ERROR_CHECK(eink_init(&s_eink));
        s_initialized = true;
    }
}

/* ── public API ─────────────────────────────────────────────────────────── */

void display_render(const dashboard_data_t *data)
{
    ensure_init();
    memset(bw_buf,  0xFF, sizeof(bw_buf));   /* all white */
    memset(red_buf, 0x00, sizeof(red_buf));  /* no red    */

    /* Derive percentages from raw counts */
    int claude_ses = (data->claude.five_hour.limit > 0)
        ? data->claude.five_hour.used * 100 / data->claude.five_hour.limit : 0;
    int claude_wk  = (data->claude.weekly.limit > 0)
        ? data->claude.weekly.used  * 100 / data->claude.weekly.limit  : 0;
    int codex_ses  = (data->codex.daily_limit > 0)
        ? data->codex.daily_used    * 100 / data->codex.daily_limit    : 0;

    bool offline    = data->offline || data->stale;
    bool deps_alert = data->github.dependabot > 0;
    bool auth_err   = data->claude.auth_error;

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
        icon_sync(251, 4);
        /* clock right-aligned, ends at x=290 */
        draw_str(290 - str_w(data->updated_at), 5, data->updated_at, 0);
    }

    /* Header bottom hairline at y=15 */
    hline(2, 15, 292);

    /* Column divider at x=118, y=16..125 */
    vline(118, 16, 110);

    /* ── Left column — icon rows ── */
    {
        char v[8];

        /* Row 1: ISSUES @ y=20 */
        snprintf(v, sizeof(v), "%d", data->github.issues);
        draw_icon_row(20, icon_issue, 0, "ISSUES", v, 0);

        /* Row 2: PRs @ y=40 */
        snprintf(v, sizeof(v), "%d", data->github.prs);
        draw_icon_row(40, icon_pr, 0, "PRs", v, 0);

        /* Row 3: DEPS @ y=60 */
        if (deps_alert)
            snprintf(v, sizeof(v), "%d!", data->github.dependabot);
        else
            snprintf(v, sizeof(v), "%d", data->github.dependabot);
        draw_icon_row(60, icon_shield, deps_alert, "DEPS", v, deps_alert);

        /* Row 4: AUTH @ y=80 */
        draw_icon_row(80, icon_key, auth_err, "AUTH",
                      auth_err ? "ERR" : "OK", auth_err);
    }

    /* Footer left: repo name, bottom-aligned */
    draw_str(6, 118, "weact/devdash", 0);

    /* ── Right column — provider bars ── */
    draw_provider(124, 18, "CLAUDE", claude_ses, claude_wk);
    draw_provider(124, 66, "CODEX",  codex_ses,  0);

    /* Footer right */
    if (offline) {
        draw_str(124, 114, "no sync", 0);
    } else {
        char upd[40];
        snprintf(upd, sizeof(upd), "upd %s", data->updated_at);
        draw_str(124, 114, upd, 0);
        draw_str(290 - str_w("next 5m"), 114, "next 5m", 0);
    }

    /* ── Refresh mode ──
     * The first refresh after power-on MUST be FULL_COLOR. BW_FAST/partial is
     * a differential LUT relative to the previously displayed frame; on a cold
     * boot that prior state is undefined, so a fast refresh produces garbage
     * (typically a near-solid black smear). */
    bool need_red = deps_alert || auth_err || offline
                    || claude_ses > 80 || claude_wk > 80 || codex_ses > 80;
    eink_refresh_mode_t mode;
    if (need_red || !s_first_refresh_done) {
        mode = EINK_REFRESH_FULL_COLOR;
        s_last_red_state      = need_red;
        s_bw_fast_cycle_count = 0;
    } else {
        s_bw_fast_cycle_count++;
        if (s_bw_fast_cycle_count >= 10) {
            mode = EINK_REFRESH_FULL_COLOR;
            s_bw_fast_cycle_count = 0;
        } else {
            mode = EINK_REFRESH_BW_FAST;
        }
        s_last_red_state = false;
    }
    s_first_refresh_done = true;

    eink_set_framebuffer(bw_buf, red_buf);
    eink_refresh(&s_eink, mode);
    eink_sleep(&s_eink);
    display_mark_frame(offline ? DISPLAY_FRAME_OFFLINE : DISPLAY_FRAME_CONTENT);
}

void display_show_qr(const char *ssid, const char *pop)
{
    ensure_init();
    memset(bw_buf,  0xFF, sizeof(bw_buf));
    memset(red_buf, 0x00, sizeof(red_buf));
    draw_str(6, 10, "Provision WiFi:", 0);

    char line[40];
    snprintf(line, sizeof(line), "SSID: %s", ssid ? ssid : "devdash-XXXX");
    draw_str(6, 30, line, 0);
    snprintf(line, sizeof(line), "Pass: %s", pop ? pop : "(see docs)");
    draw_str(6, 50, line, 0);
    draw_str(6, 70, "Open http://192.168.4.1", 0);
    draw_str(6, 90, "USB Improv still works", 0);

    eink_set_framebuffer(bw_buf, red_buf);
    eink_refresh(&s_eink, EINK_REFRESH_FULL_COLOR);
    eink_sleep(&s_eink);
    s_first_refresh_done = true;
    display_mark_frame(DISPLAY_FRAME_QR);
}

void display_show_offline(void)
{
    if (display_should_skip_offline_refresh()) {
        ESP_LOGI(TAG, "Skipping offline refresh; offline frame already shown");
        return;
    }

    ensure_init();
    memset(bw_buf,  0xFF, sizeof(bw_buf));
    memset(red_buf, 0x00, sizeof(red_buf));
    icon_cross_sync(6, 4);
    draw_str(19, 5, "OFFLINE", 1);
    hline(2, 15, 292);
    draw_str(6, 30, "API unreachable", 0);
    eink_set_framebuffer(bw_buf, red_buf);
    eink_refresh(&s_eink,
                 s_first_refresh_done ? EINK_REFRESH_BW_FAST
                                      : EINK_REFRESH_FULL_COLOR);
    eink_sleep(&s_eink);
    s_first_refresh_done = true;
    display_mark_frame(DISPLAY_FRAME_OFFLINE);
}
