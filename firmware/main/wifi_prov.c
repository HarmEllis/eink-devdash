#include "wifi_prov.h"
#include "captive_dns.h"
#include "boot_button.h"
#include "display.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_pm.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "runtime_policy.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>

static const char *TAG = "wifi_net";

static EventGroupHandle_t s_wifi_events;
#define BIT_PROV_DONE     BIT2
#define PORTAL_BODY_MAX   (24 * 1024)   /* hard cap for POST /save body */

static char s_portal_ssid[32];
static char s_portal_password[16];

static const char *ap_event_name(int32_t event_id)
{
    switch (event_id) {
    case WIFI_EVENT_AP_START:           return "started";
    case WIFI_EVENT_AP_STOP:            return "stopped";
    case WIFI_EVENT_AP_STACONNECTED:    return "client-joined";
    case WIFI_EVENT_AP_STADISCONNECTED: return "client-left";
    default:                            return "other";
    }
}

static void provisioning_wifi_event(void *arg, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base != WIFI_EVENT) return;

    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *event = event_data;
        ESP_LOGI(TAG, "SoftAP event=%s station=" MACSTR " aid=%u",
                 ap_event_name(event_id), MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *event = event_data;
        ESP_LOGI(TAG, "SoftAP event=%s station=" MACSTR " aid=%u reason=%u",
                 ap_event_name(event_id), MAC2STR(event->mac), event->aid,
                 event->reason);
    } else if (event_id == WIFI_EVENT_AP_START ||
               event_id == WIFI_EVENT_AP_STOP) {
        ESP_LOGI(TAG, "SoftAP event=%s", ap_event_name(event_id));
    }
}

/* Parsed form state. Mirrors the V4 form-name contract — see
 * design_handoff_eink_v4_provisioning/README.md §Form-name contract. */
typedef struct {
    bool     present;
    uint32_t id;            /* wN_aK_i — 0 for new slots */
    bool     enabled;       /* wN_aK_on */
    bool     cleartok;      /* wN_aK_cleartok */
    char     url[DASH_API_URL_MAX];
    char     token[DASH_DEVICE_TOKEN_MAX];
    bool     tok_present;   /* did the form carry a non-empty wN_aK_tok? */
    bool     overflow;      /* a field on this row got truncated by form_decode */
} api_form_t;

typedef struct {
    bool     present;
    uint32_t id;            /* wN_i — 0 for new slots */
    bool     enabled;       /* wN_on */
    bool     clearpw;       /* wN_clearpw */
    char     ssid[DASH_SSID_MAX + 1];
    char     password[DASH_WIFI_PASSWORD_MAX + 1];
    bool     pwd_present;
    bool     overflow;      /* SSID or password got truncated by form_decode */
    bool     q_on;          /* wN_q_on — quiet hours enabled */
    bool     q_start_present;
    bool     q_end_present;
    int      q_start;       /* wN_q_start — minutes since midnight, or -1 */
    int      q_end;         /* wN_q_end   — minutes since midnight, or -1 */
    api_form_t apis[MAX_APIS_PER_NETWORK];
} net_form_t;

typedef struct {
    net_form_t nets[MAX_WIFI_NETWORKS];
    int        iv;
    bool       iv_present;
    uint8_t    pv;         /* Display panel variant (eink_panel_variant_t). */
    bool       pv_present; /* True iff a valid `pv` field was in the form. */
    int        mp;         /* BW per-region partial cap (max_partials). */
    bool       mp_present; /* True iff an `mp` field was in the form. */
    char       country[4]; /* WiFi regulatory country code (`cc`). */
    bool       cc_present; /* True iff a `cc` field was in the form. */
} portal_form_t;

/* Connection is driven explicitly by wifi_roam_connect (esp_wifi_connect on the
 * SSID we pick after scanning), using credentials from cfg_v2 — the app owns
 * storage. We intentionally do NOT auto-connect on STA_START or auto-retry on
 * STA_DISCONNECTED here: doing so kicks off a connect with whatever config the
 * driver currently holds and blocks the scan path with
 * "STA is connecting, scan are not allowed!". */

esp_err_t wifi_net_init(void)
{
    if (!s_wifi_events) s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    /* RAM-only driver storage, set FIRST — before any WiFi setter that could
     * persist configuration (esp_wifi_set_country_code below writes flash
     * under the default WIFI_STORAGE_FLASH). The app owns all credentials in
     * the NVS config and reconfigures the driver from it on every boot/wake
     * (wifi_roam also runs with WIFI_STORAGE_RAM). FLASH storage would let
     * the driver mirror state into the nvs.net80211 namespace — dead weight
     * in the same 24 KB partition the per-network config blobs need headroom
     * in; the v5→v6 migration erases that namespace once to reclaim it. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    /* Set the regulatory domain so channels 1-13 are usable with active scan.
     * The ESP32 default ("01"/world-safe) makes channels 12-13 passive-only, so
     * an active scan cannot reliably find an AP on channel 13 — a router on
     * auto-channel that lands on 13 then appears unreachable (auth errors). The
     * country is the portal-chosen value (persisted in NVS), falling back to the
     * build-stamped default. Must run after esp_wifi_init(). */
    char country[4];
    storage_get_wifi_country(country, sizeof(country));
    /* Only call the setter when the active domain actually differs. With
     * WIFI_STORAGE_RAM (set above) the call no longer touches flash, but it
     * is still driver work on every deep-sleep refresh wake, so keep
     * skipping the no-op case. */
    wifi_country_t active = {0};
    /* Re-apply when the active code differs OR the policy is not AUTO: we always
     * want WIFI_COUNTRY_POLICY_AUTO (802.11d) from set_country_code(.., true), so
     * a stale MANUAL policy from an older build / another setter is repaired
     * once, while matching code+policy still skips the write on normal wakes. */
    bool need_set = esp_wifi_get_country(&active) != ESP_OK ||
                    strncmp(active.cc, country, 2) != 0 ||
                    active.policy != WIFI_COUNTRY_POLICY_AUTO;
    if (need_set) {
        ESP_LOGI(TAG, "Setting WiFi country code: %s", country);
        /* NON-fatal: an unsupported code (crafted POST, stale value, or a bad
         * CONFIG default) makes esp_wifi_set_country_code() return
         * ESP_ERR_INVALID_ARG. ESP_ERROR_CHECK would abort on every boot —
         * before the portal is reachable — bricking the device. Log and
         * continue on the driver's default domain instead. */
        esp_err_t cc_err = esp_wifi_set_country_code(country, true);
        if (cc_err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_set_country_code(%s) failed: %s — using driver default",
                     country, esp_err_to_name(cc_err));
        }
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    return ESP_OK;
}

bool wifi_net_is_provisioned(void)
{
    /* Heap-allocate to keep the ~7 KB cfg off the caller's stack — see the
     * SoftAP crash brief in .co-develop/ for the cautionary tale. */
    dash_config_v2_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) return false;
    storage_load_v2(cfg);
    bool ok = cfg->network_count > 0 && cfg->networks[0].ssid[0] != '\0';
    free(cfg);
    return ok;
}

/* Build the SoftAP SSID from the last two MAC octets — stable per device,
 * matches the QR string contract `devdash-XXXX`. */
static void derive_ap_ssid(char *ssid, size_t ssid_sz)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(ssid, ssid_sz, "devdash-%02X%02X", mac[4], mac[5]);
}

void wifi_net_get_prov_info(char *ssid, size_t ssid_sz,
                            char *pop, size_t pop_sz)
{
    derive_ap_ssid(ssid, ssid_sz);
    /* Random 12-char alphanumeric, generated once at first boot and
     * persisted in NVS via storage. ADR-0002 records the rationale. */
    if (pop && pop_sz >= 13) {
        esp_err_t err = storage_get_or_init_ap_password(pop, pop_sz);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "AP password load/init failed: %d", err);
            snprintf(pop, pop_sz, "errorpwd0000");
        }
    } else if (pop && pop_sz > 0) {
        pop[0] = '\0';
    }
}

size_t wifi_net_get_wifi_qr(char *out, size_t out_sz)
{
    char ssid[32];
    char pwd[16];
    derive_ap_ssid(ssid, sizeof(ssid));
    if (storage_get_or_init_ap_password(pwd, sizeof(pwd)) != ESP_OK) {
        pwd[0] = '\0';
    }
    /* SSID is `devdash-XXXX` and password is alnum — neither contains the
     * WiFi-QR-spec reserved characters (`\:,;"`), so no escaping needed. */
    int n = snprintf(out, out_sz, "WIFI:T:WPA;S:%s;P:%s;;", ssid, pwd);
    if (n < 0) return 0;
    return (size_t)n;
}

/* ── form decoding ───────────────────────────────────────────────────────── */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode an application/x-www-form-urlencoded value into dst. Returns true if
 * the decoded value fit; returns false if dst was too small to hold the full
 * decoded payload (in which case the value is silently truncated to fit, and
 * validation must reject this field rather than silently storing the prefix).
 */
static bool form_decode(char *dst, size_t dst_sz, const char *src, size_t src_len)
{
    size_t out = 0;
    bool ok = true;
    for (size_t i = 0; i < src_len; i++) {
        char c;
        if (src[i] == '+') {
            c = ' ';
        } else if (src[i] == '%' && i + 2 < src_len) {
            int hi = hex_value(src[i + 1]);
            int lo = hex_value(src[i + 2]);
            if (hi < 0 || lo < 0) continue;
            c = (char)((hi << 4) | lo);
            i += 2;
        } else {
            c = src[i];
        }
        if (out + 1 < dst_sz) {
            dst[out++] = c;
        } else {
            ok = false;   /* truncating */
        }
    }
    dst[out] = '\0';
    return ok;
}

/* Match `wN_*` / `wN_aK_*` prefixes. Returns 0..MAX-1 on success, -1 on
 * malformed keys. */
static int dec_digit(char c) { return (c >= '0' && c <= '9') ? c - '0' : -1; }

/* Parse a decoded "HH:MM" time-input value into minutes since midnight, or -1
 * when malformed / out of range. Enforces the exact 5-char "HH:MM" shape so a
 * crafted POST like "12:xx" or "1x:30" cannot slip past as a valid time
 * (atoi would silently accept the digit prefix). */
static int parse_hhmm(const char *s)
{
    if (strlen(s) != 5 || s[2] != ':') return -1;
    int h1 = dec_digit(s[0]), h0 = dec_digit(s[1]);
    int m1 = dec_digit(s[3]), m0 = dec_digit(s[4]);
    if (h1 < 0 || h0 < 0 || m1 < 0 || m0 < 0) return -1;
    int hh = h1 * 10 + h0;
    int mm = m1 * 10 + m0;
    if (hh > 23 || mm > 59) return -1;
    return hh * 60 + mm;
}

/* Apply one (key, value) pair to the form state. Keys follow the V4 contract:
 *
 *   iv
 *   wN_i               wN_on            wN_ssid           wN_pass
 *   wN_clearpw
 *   wN_aK_i            wN_aK_on         wN_aK_url
 *   wN_aK_tok          wN_aK_cleartok
 */
static void apply_field(portal_form_t *form,
                        const char *key, size_t klen,
                        const char *val, size_t vlen)
{
    if (klen == 2 && key[0] == 'i' && key[1] == 'v') {
        char buf[8] = {0};
        form_decode(buf, sizeof(buf), val, vlen);
        form->iv = atoi(buf);
        form->iv_present = true;
        return;
    }
    if (klen == 2 && key[0] == 'p' && key[1] == 'v') {
        char buf[8] = {0};
        form_decode(buf, sizeof(buf), val, vlen);
        int pv = atoi(buf);
        if (pv == EINK_PANEL_WEACT_29_BWR || pv == EINK_PANEL_WEACT_29_BW) {
            form->pv = (uint8_t)pv;
            form->pv_present = true;
        }
        return;
    }
    if (klen == 2 && key[0] == 'm' && key[1] == 'p') {
        char buf[8] = {0};
        form_decode(buf, sizeof(buf), val, vlen);
        form->mp = atoi(buf);   /* range-validated at apply time */
        form->mp_present = true;
        return;
    }
    if (klen == 2 && key[0] == 'c' && key[1] == 'c') {
        form_decode(form->country, sizeof(form->country), val, vlen);
        form->cc_present = true;   /* validated at save time */
        return;
    }
    if (klen < 4 || key[0] != 'w') return;
    int n = dec_digit(key[1]);
    if (n < 0 || n >= MAX_WIFI_NETWORKS || key[2] != '_') return;
    net_form_t *net = &form->nets[n];
    net->present = true;

    const char *rest = key + 3;
    size_t rlen = klen - 3;

    /* Network-level fields */
    if (rlen == 1 && rest[0] == 'i') {
        char buf[16] = {0};
        form_decode(buf, sizeof(buf), val, vlen);
        net->id = (uint32_t)strtoul(buf, NULL, 10);
        return;
    }
    if (rlen == 2 && rest[0] == 'o' && rest[1] == 'n') {
        net->enabled = true;
        return;
    }
    if (rlen == 4 && strncmp(rest, "ssid", 4) == 0) {
        if (!form_decode(net->ssid, sizeof(net->ssid), val, vlen)) net->overflow = true;
        return;
    }
    if (rlen == 4 && strncmp(rest, "pass", 4) == 0) {
        if (!form_decode(net->password, sizeof(net->password), val, vlen)) net->overflow = true;
        net->pwd_present = net->password[0] != '\0';
        return;
    }
    if (rlen == 7 && strncmp(rest, "clearpw", 7) == 0) {
        net->clearpw = true;
        return;
    }
    if (rlen == 4 && strncmp(rest, "q_on", 4) == 0) {
        net->q_on = true;
        return;
    }
    if (rlen == 7 && strncmp(rest, "q_start", 7) == 0) {
        char b[8] = {0};
        form_decode(b, sizeof(b), val, vlen);
        net->q_start = parse_hhmm(b);
        net->q_start_present = true;
        return;
    }
    if (rlen == 5 && strncmp(rest, "q_end", 5) == 0) {
        char b[8] = {0};
        form_decode(b, sizeof(b), val, vlen);
        net->q_end = parse_hhmm(b);
        net->q_end_present = true;
        return;
    }

    /* API-level: rest must start with `aK_…` */
    if (rlen < 3 || rest[0] != 'a') return;
    int k = dec_digit(rest[1]);
    if (k < 0 || k >= MAX_APIS_PER_NETWORK || rest[2] != '_') return;
    api_form_t *api = &net->apis[k];
    api->present = true;

    const char *sub = rest + 3;
    size_t slen = rlen - 3;

    if (slen == 1 && sub[0] == 'i') {
        char buf[16] = {0};
        form_decode(buf, sizeof(buf), val, vlen);
        api->id = (uint32_t)strtoul(buf, NULL, 10);
    } else if (slen == 2 && sub[0] == 'o' && sub[1] == 'n') {
        api->enabled = true;
    } else if (slen == 3 && strncmp(sub, "url", 3) == 0) {
        if (!form_decode(api->url, sizeof(api->url), val, vlen)) api->overflow = true;
    } else if (slen == 3 && strncmp(sub, "tok", 3) == 0) {
        if (!form_decode(api->token, sizeof(api->token), val, vlen)) api->overflow = true;
        api->tok_present = api->token[0] != '\0';
    } else if (slen == 8 && strncmp(sub, "cleartok", 8) == 0) {
        api->cleartok = true;
    }
}

static void parse_form_body(char *body, portal_form_t *form)
{
    char *p = body;
    char *body_end = body + strlen(body);
    while (*p) {
        char *key = p;
        char *eq  = strchr(key, '=');
        char *amp = strchr(key, '&');
        if (!eq || (amp && amp < eq)) {
            if (!amp) break;
            p = amp + 1;
            continue;
        }
        char *val = eq + 1;
        char *end = amp ? amp : body_end;
        apply_field(form, key, (size_t)(eq - key), val, (size_t)(end - val));
        if (!amp) break;
        p = amp + 1;
    }
}

/* ── HTML attribute escape ───────────────────────────────────────────────── */
static void html_escape(const char *src, char *dst, size_t dst_sz)
{
    size_t out = 0;
    for (size_t i = 0; src[i] && out + 6 < dst_sz; i++) {
        char c = src[i];
        const char *rep = NULL;
        switch (c) {
            case '&':  rep = "&amp;";  break;
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            case '"':  rep = "&quot;"; break;
            case '\'': rep = "&#39;";  break;
        }
        if (rep) { size_t l = strlen(rep); memcpy(dst + out, rep, l); out += l; }
        else dst[out++] = c;
    }
    dst[out] = '\0';
}

/* ── portal HTML emission ────────────────────────────────────────────────── */

#define CHUNK(req, s)  httpd_resp_sendstr_chunk((req), (s))

static const char V4_STYLE[] =
"*,*::before,*::after{box-sizing:border-box}html,body{margin:0;padding:0}"
"body{font:15px/1.45 -apple-system,BlinkMacSystemFont,\"Segoe UI\",Roboto,sans-serif;"
"color:#1c1f24;background:#f4f5f7;min-height:100vh;-webkit-text-size-adjust:100%;"
"padding-bottom:96px}"
".topbar{position:sticky;top:0;z-index:5;background:#fff;border-bottom:1px solid #e3e5e8;"
"padding:14px 16px;display:flex;align-items:center;justify-content:space-between;gap:12px}"
".topbar .name{font-weight:600;font-size:15px;letter-spacing:-0.01em}"
".topbar .name .glyph{display:inline-block;width:10px;height:10px;background:#1c1f24;"
"margin-right:6px;vertical-align:0;border-radius:1px;position:relative}"
".topbar .name .glyph::after{content:\"\";position:absolute;inset:2px;background:#fff}"
".topbar .ap{display:inline-flex;align-items:center;gap:6px;font-size:12px;color:#2a7a3a;"
"font-weight:500;background:#e8f5ec;border:1px solid #cfe7d6;padding:4px 8px;border-radius:99px}"
".topbar .ap .dot{width:6px;height:6px;border-radius:50%;background:#2a7a3a}"
".nojs-note{margin:12px 16px 0;padding:10px 12px;border:1px solid #d9dde3;border-radius:10px;"
"background:#fff;color:#4b5563;font-size:12px}"
"main{padding:16px;max-width:560px;margin:0 auto}"
"section.block{margin:0 0 20px}"
"section.block>h2{display:flex;align-items:baseline;gap:8px;font-size:12px;font-weight:600;"
"text-transform:uppercase;letter-spacing:0.06em;color:#6b7280;margin:0 0 6px;padding:0 4px}"
".count{font:600 11px/1 ui-monospace,\"SF Mono\",Menlo,monospace;letter-spacing:0.04em;"
"color:#4b5563;background:#fff;border:1px solid #e3e5e8;padding:2px 6px;border-radius:99px;text-transform:none}"
".blockhint{font-size:12px;color:#6b7280;margin:-2px 4px 10px}"
".card{background:#fff;border:1px solid #e3e5e8;border-radius:12px;margin-bottom:10px}"
".card-head,.api-row{display:flex;align-items:center;gap:10px;padding:12px 14px}"
".card-body{padding:4px 14px 12px;border-top:1px solid #eef0f3}"
".slot-label{font-weight:600;font-size:14px;color:#1c1f24;letter-spacing:-0.005em}"
".order{display:inline-block;font:600 11px/1 ui-monospace,\"SF Mono\",Menlo,monospace;"
"color:#6b7280;background:#f1f2f4;border-radius:4px;padding:3px 5px;margin-right:8px;vertical-align:1px}"
".grow{flex:1 1 auto;min-width:0}"
".pill{font-size:11px;font-weight:600;letter-spacing:0.04em;text-transform:uppercase;color:#6b7280;"
"background:#f1f2f4;padding:3px 8px;border-radius:99px;white-space:nowrap}"
".pill.saved{color:#2a4a7a;background:#e6eef9}.pill.empty{color:#6b7280;background:#f1f2f4}"
".pill.error{color:#8a2a2a;background:#fdecec}.pill.off{color:#6b7280;background:#ececef}"
".card .pill.saved,.card .pill.empty,.card .pill.error,.api-list>li .pill.saved,"
".api-list>li .pill.empty,.api-list>li .pill.error{display:none}"
".card[data-saved=\"1\"]:has(.card-on:checked) .pill.saved{display:inline-block}"
".card[data-saved=\"0\"]:has(.card-on:checked) .pill.empty{display:inline-block}"
".card:has(.card-on:not(:checked)) .pill.off{display:inline-block}"
".card.has-error:has(.card-on:checked) .pill.saved,.card.has-error:has(.card-on:checked) .pill.empty{display:none}"
".card.has-error:has(.card-on:checked) .pill.error{display:inline-block}"
".card.disabled,.card:has(.card-on:not(:checked)){background:#fafbfc;border-color:#ebedf0}"
".card.disabled .card-body,.card:has(.card-on:not(:checked)) .card-body{display:none}"
".card.disabled .slot-label,.card:has(.card-on:not(:checked)) .slot-label{color:#6b7280;font-weight:500}"
"label.field{display:block;margin:8px 0}"
"label.field>.lab{display:block;font-size:12px;color:#4b5563;margin-bottom:4px}"
"input[type=text],input[type=password],input[type=number],input[type=url]{"
"width:100%;min-height:44px;padding:10px 12px;font:inherit;font-size:15px;color:#1c1f24;"
"background:#fff;border:1px solid #c6cad0;border-radius:8px;outline:none;-webkit-appearance:none;appearance:none}"
"input:focus{border-color:#2563eb;box-shadow:0 0 0 3px rgba(37,99,235,.15)}"
"input.invalid{border-color:#c83737;background:#fff5f5}"
".err{color:#a02020;font-size:12px;margin-top:4px}.err code{font-family:ui-monospace,\"SF Mono\",Menlo,monospace}"
".toggle{position:relative;display:inline-flex;align-items:center;cursor:pointer;user-select:none;flex:0 0 auto}"
".toggle input{position:absolute;opacity:0;pointer-events:none}"
".toggle .track{width:38px;height:22px;background:#c6cad0;border-radius:99px;position:relative;transition:background .15s}"
".toggle .track::after{content:\"\";position:absolute;top:2px;left:2px;width:18px;height:18px;"
"background:#fff;border-radius:50%;transition:left .15s;box-shadow:0 1px 2px rgba(0,0,0,.2)}"
".toggle input:checked+.track{background:#2563eb}"
".toggle input:checked+.track::after{left:16px}"
".secret{display:flex;gap:8px;align-items:stretch}"
".secret input{flex:1}"
".secret .showbtn{background:#f1f2f4;border:1px solid #cbd0d6;border-radius:8px;"
"padding:0 12px;font:inherit;font-size:13px;cursor:pointer;color:#1c1f24}"
".check{display:flex;align-items:center;gap:8px;margin-top:6px;font-size:13px;color:#4b5563}"
".api-section{margin-top:14px;border-top:1px solid #eef0f3;padding-top:12px}"
".quiet-section{margin-top:14px;border-top:1px solid #eef0f3;padding-top:12px}"
".quiet-section .hint{color:#6b7280;font-weight:400}"
".quiet-times{display:flex;gap:12px}"
".quiet-times label.field{flex:1;margin:8px 0 0}"
"input[type=time]{width:100%;padding:9px 10px;border:1px solid #cdd2d9;"
"border-radius:8px;font-size:15px;background:#fff;color:#1c1f24}"
".api-head{display:flex;align-items:center;justify-content:space-between;margin-bottom:8px}"
".api-head .title{font-size:12px;font-weight:600;letter-spacing:0.06em;text-transform:uppercase;color:#6b7280}"
".api-hint{margin:0 0 8px;color:#6b7280;font-size:12px}"
".movebtn{background:#f1f2f4;border:1px solid #cbd0d6;border-radius:8px;width:34px;"
"min-height:34px;padding:0;font-size:11px;line-height:1;cursor:pointer;color:#1c1f24;flex:0 0 auto}"
".movebtn:disabled{opacity:.35;cursor:default}"
".api-list{list-style:none;margin:0;padding:0}"
".api-list>li{background:#fafbfc;border:1px dashed #cbd0d6;border-radius:10px;margin-bottom:8px}"
".api-list>li .api-body{padding:0 12px 12px}"
".apititle{font-weight:600;font-size:13px;color:#1c1f24}"
".api-list>li[data-saved=\"1\"]:has(.api-on:checked) .pill.saved{display:inline-block}"
".api-list>li[data-saved=\"0\"]:has(.api-on:checked) .pill.empty{display:inline-block}"
".api-list>li:has(.api-on:not(:checked)) .pill.off{display:inline-block}"
".api-list>li.has-error:has(.api-on:checked) .pill.saved,.api-list>li.has-error:has(.api-on:checked) .pill.empty{display:none}"
".api-list>li.has-error:has(.api-on:checked) .pill.error{display:inline-block}"
".api-list>li.disabled,.api-list>li:has(.api-on:not(:checked)){background:#f4f5f7}"
".api-list>li.disabled .api-body,.api-list>li:has(.api-on:not(:checked)) .api-body{display:none}"
".api-list>li.disabled .apititle,.api-list>li:has(.api-on:not(:checked)) .apititle{color:#6b7280;font-weight:400}"
".interval-grid{display:grid;grid-template-columns:1fr 86px;gap:10px 12px;align-items:center;"
"background:#fff;border:1px solid #e3e5e8;border-radius:12px;padding:12px}"
".interval-grid .full{grid-column:1/-1}.num-wrap{position:relative}.num-wrap input{padding-right:36px}.unit{position:absolute;right:10px;top:50%;transform:translateY(-50%);font-size:12px;color:#6b7280}"
".savebar{position:fixed;left:0;right:0;bottom:0;background:#fff;border-top:1px solid #e3e5e8;"
"padding:12px 16px;z-index:6}"
".savebar .inner{max-width:560px;margin:0 auto}"
".savebar .summary{display:none;background:#fdecec;border:1px solid #f3b9b9;color:#7a1f1f;"
"padding:8px 12px;border-radius:8px;font-size:13px;margin-bottom:8px}"
"body.has-errors .savebar .summary{display:block}"
".savebar button{width:100%;min-height:48px;font:inherit;font-size:16px;font-weight:600;"
"background:#1c1f24;color:#fff;border:0;border-radius:10px;cursor:pointer}"
"#page-confirm{display:none;min-height:100vh;align-items:center;justify-content:center;"
"flex-direction:column;text-align:center;padding:20px}"
"body.saved>main,body.saved>form,body.saved>header.topbar,body.saved>.savebar{display:none}"
"body.saved>#page-confirm{display:flex}"
".spinner{width:36px;height:36px;border:3px solid #e3e5e8;border-top-color:#2563eb;"
"border-radius:50%;animation:spin .9s linear infinite}"
"@keyframes spin{to{transform:rotate(360deg)}}"
".confirm h1{margin:16px 0 8px;font-size:18px}"
".confirm p{margin:0;color:#4b5563;font-size:14px;max-width:34ch}"
".confirm .done-check{display:none;width:36px;height:36px;background:#2a7a3a;"
"border-radius:50%;color:#fff;align-items:center;justify-content:center;font-size:20px}"
".confirm.done .spinner{display:none}"
".confirm.done .done-check{display:inline-flex}";

static const char V4_JS[] =
"<script>"
"function tgl(el,sel,cls){var c=el.closest(sel);if(c)c.classList.toggle(cls,!el.checked);}"
"function updateCounts(){"
"var wc=document.getElementById('wifi-count');if(wc)wc.textContent=document.querySelectorAll('.card .card-on:checked').length+' / 5 active';"
"document.querySelectorAll('article.card').forEach(function(card){"
"var n=card.getAttribute('data-net');var c=card.querySelector('[data-api-count=\"'+n+'\"]');"
"if(c)c.textContent=card.querySelectorAll('.api-on:checked').length+' / 5 active';"
"});}"
"function rowOn(li){var c=li.querySelector('.api-on');return!!(c&&c.checked);}"
"function apiSibling(li,dir){var p=dir==='up'?'previousElementSibling':'nextElementSibling';"
"for(var s=li[p];s;s=s[p]){if(rowOn(s))return s;}return null;}"
"function swapRows(a,b){var p=a.parentNode,an=a.nextSibling,bn=b.nextSibling;"
"if(an===b){p.insertBefore(b,a);}else if(bn===a){p.insertBefore(a,b);}"
"else{p.insertBefore(a,bn);p.insertBefore(b,an);}}"
"function reindexApis(card){var n=card.getAttribute('data-net');"
"card.querySelectorAll('li[data-api]').forEach(function(li,k){"
"li.setAttribute('data-api',n+'-'+k);"
"li.querySelectorAll('[name]').forEach(function(i){i.name=i.name.replace(/^w\\d+_a\\d+_/,'w'+n+'_a'+k+'_');});"
"li.querySelectorAll('[data-target]').forEach(function(i){"
"i.setAttribute('data-target',i.getAttribute('data-target').replace(/^w\\d+_a\\d+_/,'w'+n+'_a'+k+'_'));});"
"var t=li.querySelector('.toggle');if(t)t.setAttribute('aria-label','enable API slot '+(k+1));"
"var o=li.querySelector('.order');if(o)o.textContent=k+1;"
"});}"
"function updateMoveBtns(){document.querySelectorAll('li[data-api]').forEach(function(li){"
"var on=rowOn(li);li.querySelectorAll('.movebtn').forEach(function(b){"
"b.disabled=!on||!apiSibling(li,b.getAttribute('data-move'));});});}"
"document.addEventListener('change',function(e){"
"var t=e.target;"
"if(t.matches('.card-on,.api-on')){"
"if(t.classList.contains('api-on'))tgl(t,'.api-list>li','disabled');"
"else tgl(t,'.card','disabled');"
"updateCounts();updateMoveBtns();"
"}"
"if(t.matches('input[data-act=\"clearpw\"],input[data-act=\"cleartok\"]')){"
"var i=document.querySelector('input[name=\"'+t.getAttribute('data-target')+'\"]');"
"if(i){i.disabled=t.checked;if(t.checked)i.value='';}"
"}"
"});"
"document.addEventListener('click',function(e){"
"if(e.target.matches('.showbtn')){"
"var inp=e.target.parentElement.querySelector('input');"
"inp.type=inp.type==='password'?'text':'password';"
"e.target.textContent=inp.type==='password'?'show':'hide';"
"}"
"if(e.target.matches('.movebtn')){"
"var li=e.target.closest('li[data-api]');"
"if(li&&rowOn(li)){"
"var dir=e.target.getAttribute('data-move');"
"var sib=apiSibling(li,dir);"
"if(sib){"
"swapRows(li,sib);"
"reindexApis(li.closest('article.card'));"
"updateMoveBtns();"
"var f=e.target.disabled?li.querySelector('.movebtn:not([disabled])'):e.target;"
"if(f)f.focus();"
"}}}"
"});"
"function syncRange(a,b){a.addEventListener('input',function(){b.value=a.value});"
"b.addEventListener('input',function(){a.value=b.value});}"
"function updateRefreshMinimum(){"
"var r=document.getElementById('iv-range');var n=document.getElementById('iv-num');"
"var h=document.getElementById('iv-hint');"
"var bw=document.querySelector('input[name=\"pv\"][value=\"1\"]:checked');"
"if(!r||!n)return;var min=bw?1:3;"
"r.min=min;n.min=min;if(Number(n.value)<min){n.value=min;r.value=min;}"
"if(h)h.textContent=min===1?'1-60 min. The 1-minute option requires the BW panel and at least 2 partial refreshes.':'3-60 min. Default 5. Red-bearing frames take about 15 s of panel refresh.';"
"}"
"window.addEventListener('DOMContentLoaded',function(){"
"var r=document.getElementById('iv-range');var n=document.getElementById('iv-num');"
"if(r&&n)syncRange(r,n);"
"var mr=document.getElementById('mp-range');var mn=document.getElementById('mp-num');"
"if(mr&&mn)syncRange(mr,mn);"
"document.querySelectorAll('input[name=\"pv\"]').forEach(function(p){p.addEventListener('change',updateRefreshMinimum);});"
"updateRefreshMinimum();"
"document.querySelectorAll('.card-on,.api-on').forEach(function(t){"
"if(t.classList.contains('api-on'))tgl(t,'.api-list>li','disabled');"
"else tgl(t,'.card','disabled');"
"});"
"updateCounts();"
"document.querySelectorAll('.movebtn').forEach(function(b){b.hidden=false;});"
"updateMoveBtns();"
"var f=document.getElementById('cfg');if(f)f.addEventListener('submit',function(e){"
"var bad=[];document.querySelectorAll('.has-error').forEach(function(x){x.classList.remove('has-error')});"
"document.querySelectorAll('input.invalid').forEach(function(x){x.classList.remove('invalid')});"
"document.querySelectorAll('.err').forEach(function(x){x.hidden=true});"
"document.querySelectorAll('article.card').forEach(function(card){"
"if(!card.querySelector('.card-on').checked)return;"
"card.querySelectorAll('li[data-api]').forEach(function(li){"
"if(!li.querySelector('.api-on').checked)return;"
"var u=li.querySelector('input[type=url]');"
"if(u&&u.value&&!/^https?:\\/\\/[^\\s]+$/.test(u.value)){bad.push([card,li,u]);}"
"});});"
"if(bad.length){e.preventDefault();bad.forEach(function(x){x[0].classList.add('has-error');x[1].classList.add('has-error');x[2].classList.add('invalid');var er=x[1].querySelector('.err');if(er)er.hidden=false;});"
"document.body.classList.add('has-errors');var s=document.getElementById('errsummary');if(s)s.innerHTML='<strong>Fix '+bad.length+' problem'+(bad.length>1?'s':'')+':</strong> API URL must start with <code>http://</code> or <code>https://</code>.';bad[0][2].focus();}"
"else document.body.classList.remove('has-errors');"
"});"
"});"
"</script>";

static void render_topbar(httpd_req_t *req)
{
    char line[256];
    snprintf(line, sizeof(line),
        "<header class=\"topbar\"><div class=\"name\">"
        "<span class=\"glyph\"></span>%s</div>"
        "<div class=\"ap\"><span class=\"dot\"></span>connected to AP</div>"
        "</header>",
        s_portal_ssid);
    CHUNK(req, line);
}

/* Render one API card under network n, slot k. Pre-renders the slot even when
 * empty so the form works without JS. The enable toggle (`wN_aK_on`) drives
 * the visual collapse via CSS `.api-list>li.disabled .body{display:none}`. */
static void render_api(httpd_req_t *req, int n, int k,
                       const dash_api_profile_t *api)
{
    bool en = api && api->enabled;
    bool saved = api && (api->id || api->api_url[0] || api->device_token[0]);
    const size_t url_esc_sz = DASH_API_URL_MAX * 6 + 1;
    char *url_esc = malloc(url_esc_sz);
    if (!url_esc) {
        CHUNK(req, "<li><div class=\"api-row\">Out of memory.</div></li>");
        return;
    }
    char buf[768];

    /* No initial `disabled` class. With JS, V4_JS adds it on load. Without
     * JS, browsers with :has() honour the pure-CSS rule above; older
     * browsers just show the body — the user can still toggle and edit. */
    snprintf(buf, sizeof(buf),
        "<li data-api=\"%d-%d\" data-saved=\"%d\">"
        "<div class=\"api-row\">"
        "<label class=\"toggle\" aria-label=\"enable API slot %d\">"
        "<input type=\"checkbox\" class=\"api-on\" name=\"w%d_a%d_on\"%s>"
        "<span class=\"track\"></span></label>"
        "<span class=\"apititle grow\"><span class=\"order\">%d</span>API endpoint</span>"
        "<span class=\"pill saved\">saved</span>"
        "<span class=\"pill empty\">empty</span>"
        "<span class=\"pill error\">error</span>"
        "<span class=\"pill off\">off</span>",
        n, k, saved ? 1 : 0,
        k + 1,
        n, k, en ? " checked" : "",
        k + 1);
    CHUNK(req, buf);

    /* Reorder arrows are JS-only: server-rendered hidden, unhidden on load.
       Rendered disabled too — rows whose toggle is off are dropped on save,
       so moving them would be a silent no-op; V4_JS keeps the disabled state
       in sync with the toggles and the row position. */
    snprintf(buf, sizeof(buf),
        "<button type=\"button\" class=\"movebtn\" data-move=\"up\" "
        "aria-label=\"move API up\" hidden disabled>&#9650;</button>"
        "<button type=\"button\" class=\"movebtn\" data-move=\"down\" "
        "aria-label=\"move API down\" hidden disabled>&#9660;</button>"
        "<input type=\"hidden\" name=\"w%d_a%d_i\" value=\"%lu\">"
        "</div><div class=\"api-body\">",
        n, k, (unsigned long)(api ? api->id : 0));
    CHUNK(req, buf);

    html_escape(api && api->api_url[0] ? api->api_url : "", url_esc, url_esc_sz);
    snprintf(buf, sizeof(buf),
        "<label class=\"field\"><span class=\"lab\">API URL</span>"
        "<input type=\"url\" name=\"w%d_a%d_url\" maxlength=\"191\" "
        "placeholder=\"http://host:3000\" inputmode=\"url\" "
        "autocapitalize=\"off\" autocomplete=\"off\" value=\"%s\">"
        "<div class=\"err\" hidden>Doesn't look like a URL - try "
        "<code>http://...</code> or <code>https://...</code></div>"
        "</label>", n, k, url_esc);
    CHUNK(req, buf);

    bool has_tok = api && api->device_token[0];
    snprintf(buf, sizeof(buf),
        "<label class=\"field\"><span class=\"lab\">Device token</span>"
        "<div class=\"secret\">"
        "<input type=\"password\" name=\"w%d_a%d_tok\" maxlength=\"64\" "
        "placeholder=\"%s\" autocomplete=\"new-password\">"
        "<button type=\"button\" class=\"showbtn\">show</button></div>"
        "%s",
        n, k, has_tok ? "current token kept" : "device token",
        has_tok ? "" : "</label>");
    CHUNK(req, buf);
    if (has_tok) {
        snprintf(buf, sizeof(buf),
            "<label class=\"check\"><input type=\"checkbox\" name=\"w%d_a%d_cleartok\" "
            "data-act=\"cleartok\" data-target=\"w%d_a%d_tok\">"
            "Clear saved token</label></label>",
            n, k, n, k);
        CHUNK(req, buf);
    }

    CHUNK(req, "</div></li>");
    free(url_esc);
}

/* Render one WiFi network card and its 5 API slots. quiet_* describe the
   per-network sleep window for this slot (minutes since local midnight). */
static void render_network(httpd_req_t *req, int n,
                           const dash_wifi_profile_t *net,
                           bool quiet_on, int quiet_start, int quiet_end)
{
    bool en = net && net->enabled;
    bool saved = net && (net->id || net->ssid[0] || net->password[0]);
    int active_apis = 0;
    if (net) {
        for (int k = 0; k < net->api_count; k++) {
            if (net->apis[k].enabled) active_apis++;
        }
    }
    char ssid_esc[DASH_SSID_MAX * 6 + 1];
    char label_esc[DASH_SSID_MAX * 6 + 32];
    char buf[DASH_SSID_MAX * 6 + 800];
    html_escape(net && net->ssid[0] ? net->ssid : "", ssid_esc, sizeof(ssid_esc));
    if (ssid_esc[0]) {
        snprintf(label_esc, sizeof(label_esc), "%s", ssid_esc);
    } else {
        snprintf(label_esc, sizeof(label_esc), "WiFi slot %d", n + 1);
    }

    snprintf(buf, sizeof(buf),
        "<article class=\"card\" data-net=\"%d\" data-saved=\"%d\">"
        "<div class=\"card-head\">"
        "<label class=\"toggle\" aria-label=\"enable WiFi slot %d\">"
        "<input type=\"checkbox\" class=\"card-on\" name=\"w%d_on\"%s>"
        "<span class=\"track\"></span></label>"
        "<span class=\"slot-label grow\"><span class=\"order\">%d</span>%s</span>"
        "<span class=\"pill saved\">saved</span>"
        "<span class=\"pill empty\">empty</span>"
        "<span class=\"pill error\">error</span>"
        "<span class=\"pill off\">off</span>"
        "<input type=\"hidden\" name=\"w%d_i\" value=\"%lu\">"
        "</div><div class=\"card-body\">",
        n, saved ? 1 : 0,
        n + 1,
        n, en ? " checked" : "",
        n + 1, label_esc,
        n, (unsigned long)(net ? net->id : 0));
    CHUNK(req, buf);

    snprintf(buf, sizeof(buf),
        "<label class=\"field\"><span class=\"lab\">SSID</span>"
        "<input type=\"text\" name=\"w%d_ssid\" maxlength=\"32\" value=\"%s\" "
        "autocapitalize=\"off\" autocomplete=\"off\">"
        "</label>", n, ssid_esc);
    CHUNK(req, buf);

    bool has_pwd = net && net->password[0];
    snprintf(buf, sizeof(buf),
        "<label class=\"field\"><span class=\"lab\">Password</span>"
        "<div class=\"secret\">"
        "<input type=\"password\" name=\"w%d_pass\" maxlength=\"64\" "
        "placeholder=\"%s\" autocomplete=\"new-password\">"
        "<button type=\"button\" class=\"showbtn\">show</button></div>"
        "%s",
        n, has_pwd ? "current password kept" : "required for new networks",
        has_pwd ? "" : "</label>");
    CHUNK(req, buf);
    if (has_pwd) {
        snprintf(buf, sizeof(buf),
            "<label class=\"check\"><input type=\"checkbox\" name=\"w%d_clearpw\" "
            "data-act=\"clearpw\" data-target=\"w%d_pass\">"
            "Clear saved password</label></label>",
            n, n);
        CHUNK(req, buf);
    }

    /* Quiet hours: a fresh slot pre-fills 23:00–06:00 as a sensible default so
       the time inputs are not 00:00–00:00 when the user first enables it. */
    int qs = quiet_start, qe = quiet_end;
    if (!quiet_on && qs == 0 && qe == 0) { qs = 23 * 60; qe = 6 * 60; }
    snprintf(buf, sizeof(buf),
        "<div class=\"quiet-section\">"
        "<label class=\"check\"><input type=\"checkbox\" name=\"w%d_q_on\"%s> "
        "Quiet hours <span class=\"hint\">- skip updates between these times</span>"
        "</label>"
        "<div class=\"quiet-times\">"
        "<label class=\"field\"><span class=\"lab\">Sleep from</span>"
        "<input type=\"time\" name=\"w%d_q_start\" value=\"%02d:%02d\"></label>"
        "<label class=\"field\"><span class=\"lab\">until</span>"
        "<input type=\"time\" name=\"w%d_q_end\" value=\"%02d:%02d\"></label>"
        "</div></div>",
        n, quiet_on ? " checked" : "",
        n, qs / 60, qs % 60,
        n, qe / 60, qe % 60);
    CHUNK(req, buf);

    snprintf(buf, sizeof(buf),
        "<div class=\"api-section\"><div class=\"api-head\">"
        "<span class=\"title\">API endpoints</span>"
        "<span class=\"count\" data-api-count=\"%d\">%d / 5 active</span>"
        "</div>"
        "<p class=\"api-hint\">Tried top-first on a fresh start; once one "
        "works, the device keeps using it until it fails. Reorder with the "
        "arrows.</p>"
        "<ul class=\"api-list\">",
        n, active_apis);
    CHUNK(req, buf);
    for (int k = 0; k < MAX_APIS_PER_NETWORK; k++) {
        const dash_api_profile_t *api =
            (net && k < net->api_count) ? &net->apis[k] : NULL;
        render_api(req, n, k, api);
    }
    CHUNK(req, "</ul></div></div></article>");
}

/* Curated WiFi-region picker (UX shortlist, not the full allow-list). "01"
 * (worldwide) is always first as the legal everywhere-safe fallback; the rest
 * are common single-region codes. A user whose exact country is absent can stay
 * on "01" (channels 1-11 work globally) or keep an off-list-but-supported code,
 * which round-trips via render_region_block's fallback <option>. Every code here
 * must pass wifi_country_is_supported (the complete ESP-IDF allow-list). */
static const struct { const char *code; const char *name; } COUNTRY_OPTIONS[] = {
    {"01", "Worldwide (safe default)"},
    {"AU", "Australia"}, {"AT", "Austria"},   {"BE", "Belgium"},
    {"BR", "Brazil"},    {"CA", "Canada"},    {"CH", "Switzerland"},
    {"CN", "China"},     {"CZ", "Czechia"},   {"DE", "Germany"},
    {"DK", "Denmark"},   {"ES", "Spain"},     {"FI", "Finland"},
    {"FR", "France"},    {"GB", "United Kingdom"}, {"IE", "Ireland"},
    {"IN", "India"},     {"IT", "Italy"},     {"JP", "Japan"},
    {"NL", "Netherlands"}, {"NO", "Norway"},  {"NZ", "New Zealand"},
    {"PL", "Poland"},    {"PT", "Portugal"},  {"SE", "Sweden"},
    {"US", "United States"},
};

/* Render the WiFi-region <section>. Closes the preceding WiFi-networks section
 * and opens its own block; the caller emits the Display section next. `current`
 * is the saved/effective country code (already validated). */
static void render_region_block(httpd_req_t *req, const char *current)
{
    CHUNK(req,
        "</section>"
        "<section class=\"block\" id=\"region-block\">"
        "<h2>WiFi region</h2>"
        "<div class=\"blockhint\">Sets the regulatory domain. Pick your country "
        "so channels 12-13 are usable; leave on Worldwide if unsure.</div>"
        "<div style=\"background:#fff;border:1px solid #e3e5e8;border-radius:12px;"
        "padding:12px\">"
        "<label class=\"field\" style=\"margin:0\"><span class=\"lab\">Country</span>"
        "<select name=\"cc\" style=\"width:100%;min-height:44px;padding:10px 12px;"
        "font:inherit;font-size:15px;color:#1c1f24;background:#fff;"
        "border:1px solid #c6cad0;border-radius:8px\">");

    char buf[128];
    bool matched = false;
    for (size_t i = 0; i < sizeof(COUNTRY_OPTIONS) / sizeof(COUNTRY_OPTIONS[0]); i++) {
        bool sel = strcmp(COUNTRY_OPTIONS[i].code, current) == 0;
        if (sel) matched = true;
        snprintf(buf, sizeof(buf), "<option value=\"%s\"%s>%s &mdash; %s</option>",
                 COUNTRY_OPTIONS[i].code, sel ? " selected" : "",
                 COUNTRY_OPTIONS[i].code, COUNTRY_OPTIONS[i].name);
        CHUNK(req, buf);
    }
    /* A saved code outside the curated list (e.g. set via an older build) still
     * needs to round-trip, so surface it as a selected entry rather than
     * silently resetting it on the next save. */
    if (!matched) {
        char cur_esc[8];
        html_escape(current, cur_esc, sizeof(cur_esc));
        snprintf(buf, sizeof(buf), "<option value=\"%s\" selected>%s</option>",
                 cur_esc, cur_esc);
        CHUNK(req, buf);
    }
    CHUNK(req, "</select></label></div></section>");
}

static void render_portal_page_from_cfg(httpd_req_t *req,
                                        const dash_config_v2_t *cfg,
                                        const char *error)
{
    int active_networks = 0;
    for (int n = 0; n < cfg->network_count; n++) {
        if (cfg->networks[n].enabled) active_networks++;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    CHUNK(req,
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,"
        "viewport-fit=cover\">"
        "<meta name=\"color-scheme\" content=\"light\">"
        "<title>devdash setup</title><style>");
    CHUNK(req, V4_STYLE);
    CHUNK(req, "</style></head><body>");

    render_topbar(req);

    char page_buf[1024];
    if (error && error[0]) {
        char err_esc[384];
        html_escape(error, err_esc, sizeof(err_esc));
        snprintf(page_buf, sizeof(page_buf),
            "<div class=\"savebar\" style=\"position:static;border-top:0\">"
            "<div class=\"inner\"><div class=\"summary\" style=\"display:block\">"
            "<strong>Could not save:</strong> %s</div></div></div>",
            err_esc);
        CHUNK(req, page_buf);
    }

    snprintf(page_buf, sizeof(page_buf),
        "<noscript><div class=\"nojs-note\">JavaScript is off - the 5x5 slot "
        "grid is server-rendered, and saving uses a plain form post.</div></noscript>"
        "<main><form id=\"cfg\" method=\"post\" action=\"/save\" novalidate>"
        "<section class=\"block\" id=\"wifi-block\">"
        "<h2>WiFi networks <span class=\"count\" id=\"wifi-count\">%d / 5 active</span></h2>"
        "<div class=\"blockhint\">All 5 slots are reserved. Toggle a slot on to "
        "fill it in; the device connects to the strongest visible network.</div>",
        active_networks);
    CHUNK(req, page_buf);

    for (int n = 0; n < MAX_WIFI_NETWORKS; n++) {
        const dash_wifi_profile_t *net =
            (n < cfg->network_count) ? &cfg->networks[n] : NULL;
        bool q_on   = net && net->quiet_enabled;
        int  q_start = net ? net->quiet_start_min : 0;
        int  q_end   = net ? net->quiet_end_min : 0;
        render_network(req, n, net, q_on, q_start, q_end);
    }

    /* WiFi region picker (closes the WiFi-networks section, opens its own). */
    char country[4];
    storage_get_wifi_country(country, sizeof(country));
    render_region_block(req, country);

    int iv = cfg->refresh_min ? cfg->refresh_min : 5;
    /* Expose the 1-minute input server-side for every saved BW panel so the
       option does not depend on captive-portal JavaScript. Save validation
       still requires max_partials >= 2 before accepting a 1-minute interval. */
    int iv_min = dashboard_refresh_input_minimum(
        cfg->panel_variant == EINK_PANEL_WEACT_29_BW);
    snprintf(page_buf, sizeof(page_buf),
        "<section class=\"block\" id=\"display-block\">"
        "<h2>Display</h2>"
        "<div class=\"interval-grid\">"
        "<label class=\"lab full\" for=\"iv-range\" style=\"font-size:14px;color:#1c1f24\">"
        "Refresh interval <span style=\"color:#6b7280;font-weight:400\">- how often "
        "the dashboard polls.</span></label>"
        "<input type=\"range\" id=\"iv-range\" min=\"%d\" max=\"60\" step=\"1\" value=\"%d\">"
        "<div class=\"num-wrap\"><input type=\"number\" id=\"iv-num\" name=\"iv\" "
        "min=\"%d\" max=\"60\" step=\"1\" value=\"%d\"><span class=\"unit\">min</span></div>"
        "<p class=\"full\" id=\"iv-hint\" style=\"margin:0;color:#6b7280;font-size:12px\">"
        "%s</p>"
        "</div>",
        iv_min, iv, iv_min, iv,
        iv_min == DASH_REFRESH_MIN_BW_TWO_PARTIALS
            ? "1-60 min. The 1-minute option requires the BW panel and at least "
              "2 partial refreshes."
            : "3-60 min. Default 5. Red-bearing frames take about 15 s of panel refresh.");
    CHUNK(req, page_buf);

    /* BW per-region partial cap (max_partials). Mirrors the interval control. */
    int mp = cfg->max_partials ? cfg->max_partials : DASH_MAX_PARTIALS_DEFAULT;
    snprintf(page_buf, sizeof(page_buf),
        "<div class=\"interval-grid\" style=\"margin-top:14px\">"
        "<label class=\"lab full\" for=\"mp-range\" style=\"font-size:14px;color:#1c1f24\">"
        "Partial refreshes per region <span style=\"color:#6b7280;font-weight:400\">- "
        "before a full refresh.</span></label>"
        "<input type=\"range\" id=\"mp-range\" min=\"%d\" max=\"%d\" step=\"1\" value=\"%d\">"
        "<div class=\"num-wrap\"><input type=\"number\" id=\"mp-num\" name=\"mp\" "
        "min=\"%d\" max=\"%d\" step=\"1\" value=\"%d\"><span class=\"unit\">x</span></div>"
        "<p class=\"full\" style=\"margin:0;color:#6b7280;font-size:12px\">"
        "%d-%d. Default %d. BW panel only — higher means fewer full-screen flashes but "
        "more partial-refresh ghosting between them.</p>"
        "</div>",
        DASH_MAX_PARTIALS_MIN, DASH_MAX_PARTIALS_MAX, mp,
        DASH_MAX_PARTIALS_MIN, DASH_MAX_PARTIALS_MAX, mp,
        DASH_MAX_PARTIALS_MIN, DASH_MAX_PARTIALS_MAX, DASH_MAX_PARTIALS_DEFAULT);
    CHUNK(req, page_buf);

    /* Panel variant selector — two radio buttons. Checked state reflects
       the saved cfg->panel_variant so a save with the form unchanged keeps
       the previous choice. */
    int pv_bwr = (cfg->panel_variant == EINK_PANEL_WEACT_29_BWR);
    int pv_bw  = (cfg->panel_variant == EINK_PANEL_WEACT_29_BW);
    if (!pv_bwr && !pv_bw) pv_bwr = 1;
    snprintf(page_buf, sizeof(page_buf),
        "<div style=\"margin-top:14px\">"
        "<label class=\"lab full\" style=\"font-size:14px;color:#1c1f24\">Panel</label>"
        "<p style=\"margin:2px 0 8px;color:#6b7280;font-size:12px\">"
        "Pick the e-paper module you have wired up. Restart applies the "
        "selection.</p>"
        "<label style=\"display:block;margin:4px 0;font-size:13px;color:#1c1f24\">"
        "<input type=\"radio\" name=\"pv\" value=\"%d\"%s> "
        "WeAct 2.9&quot; Black/White/Red"
        "</label>"
        "<label style=\"display:block;margin:4px 0;font-size:13px;color:#1c1f24\">"
        "<input type=\"radio\" name=\"pv\" value=\"%d\"%s> "
        "WeAct 2.9&quot; Black/White"
        "</label>"
        "</div>"
        "</section>",
        (int)EINK_PANEL_WEACT_29_BWR, pv_bwr ? " checked" : "",
        (int)EINK_PANEL_WEACT_29_BW,  pv_bw  ? " checked" : "");
    CHUNK(req, page_buf);

    CHUNK(req,
        "<div class=\"savebar\"><div class=\"inner\">"
        "<div class=\"summary\" id=\"errsummary\">Fix the highlighted fields before saving.</div>"
        "<button type=\"submit\">Save &amp; restart</button>"
        "</div></div>"
        "</form></main>"

        "<div id=\"page-confirm\"><div class=\"confirm\">"
        "<div class=\"spinner\"></div><div class=\"done-check\">&#10003;</div>"
        "<h1>Saved. Device restarting&hellip;</h1>"
        "<p>Reconnect to your normal WiFi. The dashboard will be online "
        "shortly. You can close this tab now.</p>"
        "</div></div>");

    CHUNK(req, V4_JS);
    CHUNK(req, "</body></html>");
    CHUNK(req, NULL);
}

static void render_portal_page(httpd_req_t *req)
{
    /* dash_config_v2_t is ~7 KB; heap-allocate so the httpd task stack
     * stays within budget. See .co-develop/SOFTAP_DHCP_CONTEXT_SWITCH_CRASH_BRIEF.md
     * for the root-cause analysis. */
    dash_config_v2_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Out of memory.");
        return;
    }
    storage_load_v2(cfg);
    render_portal_page_from_cfg(req, cfg, NULL);
    free(cfg);
}

/* The successful POST /save response: full V4 #saved page. The spinner runs
 * for ~3s before swapping to a green check. The actual esp_restart is
 * scheduled ~4s after this response leaves, so the visual transition has
 * time to complete on-device. */
static void render_saved_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    CHUNK(req,
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>devdash saved</title><style>");
    CHUNK(req, V4_STYLE);
    CHUNK(req,
        "body{background:#f4f5f7}body{padding:0}"
        "</style></head><body class=\"saved\">"
        "<div id=\"page-confirm\"><div class=\"confirm\" id=\"c\">"
        "<div class=\"spinner\"></div>"
        "<div class=\"done-check\">&#10003;</div>"
        "<h1>Saved. Device restarting&hellip;</h1>"
        "<p>Reconnect to your normal WiFi. The dashboard will be online "
        "shortly. You can close this tab now.</p>"
        "</div></div>"
        "<script>setTimeout(function(){"
        "document.getElementById('c').classList.add('done');"
        "},3000);</script>"
        "</body></html>");
    CHUNK(req, NULL);
}

/* ── save flow ───────────────────────────────────────────────────────────── */

static void schedule_restart_cb(void *arg) { (void)arg; esp_restart(); }

static void schedule_restart(uint32_t ms)
{
    static esp_timer_handle_t timer;
    esp_timer_create_args_t args = {
        .callback = schedule_restart_cb,
        .name     = "prov_restart",
    };
    if (esp_timer_create(&args, &timer) != ESP_OK) return;
    esp_timer_start_once(timer, (uint64_t)ms * 1000ULL);
}

/* Caller owns both `prev` (the previous on-disk config, loaded by
 * handler_save) and `cfg` (the rebuilt config we write back). Splitting
 * ownership out keeps the per-request heap allocations in one place and
 * avoids stack-copying ~7 KB inside this function. */
static esp_err_t apply_form_to_cfg(const portal_form_t *form,
                                   const dash_config_v2_t *prev,
                                   dash_config_v2_t *cfg)
{
    storage_cfg_v2_defaults(cfg);

    /* Seed the next-id counter from the previous config so every new slot
     * minted during this save gets a distinct id. storage_next_profile_id
     * walks `cfg`, which is empty mid-rebuild, so it would otherwise hand
     * out the same id to every new entry. */
    uint32_t next_id = storage_next_profile_id(prev);

    /* Accept only an in-range partial cap; otherwise preserve the previous
       setting. storage_cfg_v2_normalize clamps defensively as well. */
    if (form->mp_present && form->mp >= DASH_MAX_PARTIALS_MIN &&
        form->mp <= DASH_MAX_PARTIALS_MAX) {
        cfg->max_partials = (uint8_t)form->mp;
    } else {
        cfg->max_partials = prev->max_partials ? prev->max_partials
                                               : DASH_MAX_PARTIALS_DEFAULT;
    }

    /* If the form posted a valid `pv`, take it; otherwise preserve the
       prev value so a BW device saving an unrelated WiFi/API change with
       a form that omits `pv` (old browser cache, crafted POST) does not
       silently revert to BWR. storage_cfg_v2_normalize() further clamps
       anything unexpected to BWR. */
    if (form->pv_present) {
        cfg->panel_variant = form->pv;
    } else {
        cfg->panel_variant = prev->panel_variant;
    }

    bool is_bw = cfg->panel_variant == EINK_PANEL_WEACT_29_BW;
    if (form->iv_present && form->iv >= 0 && form->iv <= UINT8_MAX &&
        dashboard_refresh_config_is_valid((uint8_t)form->iv, is_bw,
                                          cfg->max_partials)) {
        cfg->refresh_min = (uint8_t)form->iv;
    } else {
        cfg->refresh_min = prev->refresh_min ? prev->refresh_min : 5;
    }

    int out_n = 0;
    for (int n = 0; n < MAX_WIFI_NETWORKS; n++) {
        const net_form_t *nf = &form->nets[n];
        if (!nf->enabled) continue;

        /* Find the matching old network by id for secret preservation. */
        const dash_wifi_profile_t *old = NULL;
        if (nf->id != 0) {
            for (int p = 0; p < prev->network_count; p++) {
                if (prev->networks[p].id == nf->id) {
                    old = &prev->networks[p];
                    break;
                }
            }
        }

        dash_wifi_profile_t *nw = &cfg->networks[out_n];
        memset(nw, 0, sizeof(*nw));
        nw->enabled = true;
        nw->id = (nf->id && old) ? nf->id : next_id++;
        strncpy(nw->ssid, nf->ssid, sizeof(nw->ssid) - 1);

        if (nf->clearpw && !nf->pwd_present) {
            nw->password[0] = '\0';
        } else if (nf->pwd_present) {
            strncpy(nw->password, nf->password, sizeof(nw->password) - 1);
        } else if (old) {
            strncpy(nw->password, old->password, sizeof(nw->password) - 1);
        }

        int out_k = 0;
        for (int k = 0; k < MAX_APIS_PER_NETWORK; k++) {
            const api_form_t *af = &nf->apis[k];
            if (!af->enabled) continue;

            const dash_api_profile_t *aold = NULL;
            if (af->id != 0 && old) {
                for (int p = 0; p < old->api_count; p++) {
                    if (old->apis[p].id == af->id) { aold = &old->apis[p]; break; }
                }
            }

            dash_api_profile_t *ap = &nw->apis[out_k];
            memset(ap, 0, sizeof(*ap));
            ap->enabled = true;
            ap->id = (af->id && aold) ? af->id : next_id++;
            strncpy(ap->api_url, af->url, sizeof(ap->api_url) - 1);

            if (af->cleartok && !af->tok_present) {
                ap->device_token[0] = '\0';
            } else if (af->tok_present) {
                strncpy(ap->device_token, af->token, sizeof(ap->device_token) - 1);
            } else if (aold) {
                strncpy(ap->device_token, aold->device_token,
                        sizeof(ap->device_token) - 1);
            }
            out_k++;
            if (out_k >= MAX_APIS_PER_NETWORK) break;
        }
        nw->api_count = out_k;

        /* Quiet hours for this slot. Enabled only when the toggle is on and
           both times parsed to a distinct, valid minute-of-day; otherwise
           disabled. Parsed times are preserved even when disabled so toggling
           off and back on keeps the user's window. storage normalize re-clamps
           and re-checks start == end. */
        bool q_times_ok = nf->q_start_present && nf->q_end_present &&
                          nf->q_start >= 0 && nf->q_end >= 0 &&
                          nf->q_start != nf->q_end;
        nw->quiet_enabled = (nf->q_on && q_times_ok) ? 1 : 0;
        nw->quiet_start_min =
            (nf->q_start_present && nf->q_start >= 0) ? (uint16_t)nf->q_start : 0;
        nw->quiet_end_min =
            (nf->q_end_present && nf->q_end >= 0) ? (uint16_t)nf->q_end : 0;

        out_n++;
        if (out_n >= MAX_WIFI_NETWORKS) break;
    }
    cfg->network_count = out_n;
    return ESP_OK;
}

static void render_save_error(httpd_req_t *req,
                              const portal_form_t *form,
                              const char *reason,
                              const char *http_status)
{
    dash_config_v2_t *prev = calloc(1, sizeof(*prev));
    dash_config_v2_t *cfg  = calloc(1, sizeof(*cfg));
    if (!prev || !cfg) {
        free(prev); free(cfg);
        httpd_resp_set_status(req, http_status);
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_sendstr(req, reason);
        return;
    }

    storage_load_v2(prev);
    apply_form_to_cfg(form, prev, cfg);
    httpd_resp_set_status(req, http_status);
    render_portal_page_from_cfg(req, cfg, reason);
    free(prev);
    free(cfg);
}

/* ── HTTP handlers ───────────────────────────────────────────────────────── */

static esp_err_t handler_root(httpd_req_t *req)
{
    render_portal_page(req);
    return ESP_OK;
}

static esp_err_t handler_save(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > PORTAL_BODY_MAX) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "Form too large.");
        return ESP_OK;
    }

    char *body = malloc(req->content_len + 1);
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Out of memory.");
        return ESP_OK;
    }

    int received = 0;
    while (received < req->content_len) {
        int chunk = httpd_req_recv(req, body + received,
                                   req->content_len - received);
        if (chunk <= 0) { free(body); return ESP_FAIL; }
        received += chunk;
    }
    body[received] = '\0';

    /* portal_form_t is ~7 KB; keep it off the httpd task stack. */
    portal_form_t *form = calloc(1, sizeof(*form));
    if (!form) {
        free(body);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Out of memory.");
        return ESP_OK;
    }
    parse_form_body(body, form);
    free(body);

    /* Validate the whole form. On failure return a brief plain-text reason
     * — the V4 design's inline-error rendering would double the renderer
     * size; this keeps the caller informed without a full second pass. */
    const char *reason = NULL;
    if (form->mp_present &&
        (form->mp < DASH_MAX_PARTIALS_MIN || form->mp > DASH_MAX_PARTIALS_MAX)) {
        reason = "Max partial refreshes must be between 1 and 100.";
    }
    bool submitted_bw = form->pv_present &&
                        form->pv == EINK_PANEL_WEACT_29_BW;
    uint8_t submitted_partials =
        form->mp_present && form->mp >= DASH_MAX_PARTIALS_MIN &&
                form->mp <= DASH_MAX_PARTIALS_MAX
            ? (uint8_t)form->mp
            : 0;
    if (!reason && form->iv_present &&
        (form->iv < 0 || form->iv > UINT8_MAX ||
         !dashboard_refresh_config_is_valid(
             (uint8_t)form->iv, submitted_bw, submitted_partials))) {
        reason = "Refresh interval must be 3-60 minutes, or 1-60 minutes "
                 "with the BW panel and at least 2 partial refreshes.";
    }
    if (!reason && form->cc_present &&
        !wifi_country_is_supported(form->country)) {
        reason = "Pick a supported WiFi country (or Worldwide).";
    }
    for (int n = 0; n < MAX_WIFI_NETWORKS && !reason; n++) {
        const net_form_t *nf = &form->nets[n];
        if (!nf->enabled) continue;
        if (nf->overflow) {
            reason = "A WiFi field is longer than this device accepts.";
        } else if (strlen(nf->ssid) < 1 || strlen(nf->ssid) > DASH_SSID_MAX) {
            reason = "Each enabled network needs an SSID (1-32 chars).";
        } else if (nf->id == 0 &&
                   (strlen(nf->password) < 8 ||
                    strlen(nf->password) > DASH_WIFI_PASSWORD_MAX)) {
            reason = "New WiFi networks need a password of 8-64 characters.";
        } else if (nf->pwd_present &&
                   (strlen(nf->password) < 8 ||
                    strlen(nf->password) > DASH_WIFI_PASSWORD_MAX)) {
            reason = "WiFi passwords must be 8-64 characters.";
        } else if (nf->q_on &&
                   (!nf->q_start_present || !nf->q_end_present ||
                    nf->q_start < 0 || nf->q_end < 0)) {
            reason = "Quiet hours needs a valid start and end time.";
        } else if (nf->q_on && nf->q_start == nf->q_end) {
            reason = "Quiet hours start and end cannot be the same.";
        }
        for (int k = 0; k < MAX_APIS_PER_NETWORK && !reason; k++) {
            const api_form_t *af = &nf->apis[k];
            if (!af->enabled) continue;
            if (af->overflow) {
                reason = "An API URL or token is longer than this device accepts.";
            } else if (!storage_validate_api_url(af->url)) {
                reason = "Each enabled API needs a valid http:// or https:// URL.";
            } else if (af->id == 0 && !af->tok_present) {
                reason = "Each new API entry needs a device token.";
            }
        }
    }
    if (reason) {
        /* Form validation failure is a client error. */
        render_save_error(req, form, reason, "400 Bad Request");
        free(form);
        return ESP_OK;
    }

    /* Two ~7 KB cfg copies — keep both off the httpd task stack. `prev` is
     * loaded from NVS; `cfg` is rebuilt by apply_form_to_cfg. */
    dash_config_v2_t *prev = calloc(1, sizeof(*prev));
    dash_config_v2_t *cfg  = calloc(1, sizeof(*cfg));
    if (!prev || !cfg) {
        free(form); free(prev); free(cfg);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Out of memory.");
        return ESP_OK;
    }
    storage_load_v2(prev);
    apply_form_to_cfg(form, prev, cfg);

    /* Persist the WiFi region first: it is a tiny, separate NVS key and the
     * cheap write, so committing it before the ~7 KB config blob means a
     * full-NVS failure surfaces here rather than after the config "succeeded".
     * Fail closed — keep the portal open instead of restarting with the chosen
     * regulatory domain unapplied (a wrong domain can hide a ch12/13 network).
     *
     * Only write when the code actually changes from the current effective value
     * (avoids an NVS update on an unchanged save, and never pins the build
     * default as an explicit key). Record whether an explicit key existed so a
     * later config-blob failure can roll back to the EXACT prior state — restore
     * the old value, or erase the key when none existed — keeping the region key
     * and the config blob consistent without a cross-blob NVS transaction. */
    bool cc_written = false;
    bool cc_was_saved = false;
    char prev_country[4] = {0};
    if (form->cc_present) {
        storage_get_wifi_country(prev_country, sizeof(prev_country));
        if (strncmp(prev_country, form->country, 2) != 0) {
            cc_was_saved = storage_wifi_country_is_saved();
            esp_err_t cc_err = storage_set_wifi_country(form->country);
            if (cc_err != ESP_OK) {
                free(prev); free(cfg);
                ESP_LOGE(TAG, "storage_set_wifi_country(%s) failed: 0x%x (%s)",
                         form->country, cc_err, esp_err_to_name(cc_err));
                char rsn[180];
                snprintf(rsn, sizeof(rsn),
                         "Could not save the WiFi region to device storage: %s (0x%x). "
                         "The flash may be full — try again, or erase the device.",
                         esp_err_to_name(cc_err), cc_err);
                render_save_error(req, form, rsn, "500 Internal Server Error");
                free(form);
                return ESP_OK;
            }
            cc_written = true;
        }
    }

    esp_err_t err = storage_save_v2(cfg);
    free(prev); free(cfg);
    if (err != ESP_OK) {
        /* Roll back the region so a failed config save leaves NOTHING applied —
         * otherwise the next reboot would adopt the new domain while all other
         * submitted edits are lost. Restore the exact prior state. Best-effort:
         * a rollback that also fails is logged but cannot worsen the
         * already-reported error. */
        if (cc_written) {
            esp_err_t r = cc_was_saved ? storage_set_wifi_country(prev_country)
                                       : storage_clear_wifi_country();
            if (r != ESP_OK) {
                ESP_LOGE(TAG, "WiFi region rollback failed: %s",
                         esp_err_to_name(r));
            }
        }
        ESP_LOGE(TAG, "storage_save_v2 failed: 0x%x (%s)",
                 err, esp_err_to_name(err));
        /* Surface the exact NVS error as an inline banner on the re-rendered
         * portal (keeping the user's edits) instead of a bare text page. */
        char reason[200];
        snprintf(reason, sizeof(reason),
                 "Could not save to device storage: %s (0x%x). The flash may be "
                 "full — try again, or erase the device and re-provision.",
                 esp_err_to_name(err), err);
        /* NVS/flash write failure is a device/server error, not client input. */
        render_save_error(req, form, reason, "500 Internal Server Error");
        free(form);
        return ESP_OK;
    }
    free(form);

    /* End the provisioning session only after the complete configuration has
     * reached NVS. The deferred restart below may otherwise look like the USB
     * reset that interrupted the portal and must keep it latched. */
    boot_button_force_prov_clear();
    render_saved_page(req);
    /* DO NOT signal BIT_PROV_DONE here. The provisioning task holds the AP /
     * DNS / httpd open while it waits on that bit; if we signalled now,
     * main.c would tear everything down and call esp_restart in the next
     * few ms — before the phone finishes the 3 s spinner/check transition.
     * Instead we schedule a deferred esp_restart 4 s out, which is the only
     * path that actually returns control: esp_restart preempts the timeout
     * wait and reboots the chip cleanly. */
    schedule_restart(4000);
    return ESP_OK;
}

static esp_err_t handler_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t start_portal_server(httpd_handle_t *server)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.uri_match_fn     = httpd_uri_match_wildcard;
    config.max_uri_handlers = 12;
    /* Defensive margin. The request locals are heap-allocated (see
     * render_portal_page / handler_save), so 16 KB is belt-and-suspenders
     * for inner renderers and httpd internals, not the primary correctness
     * fix for the SoftAP crash. */
    config.stack_size       = 16384;
    esp_err_t err = httpd_start(server, &config);
    if (err != ESP_OK) return err;

    httpd_uri_t root      = {.uri = "/",                       .method = HTTP_GET,  .handler = handler_root};
    httpd_uri_t save      = {.uri = "/save",                   .method = HTTP_POST, .handler = handler_save};
    httpd_uri_t cap_andr  = {.uri = "/generate_204",           .method = HTTP_GET,  .handler = handler_redirect};
    httpd_uri_t cap_ios   = {.uri = "/hotspot-detect.html",    .method = HTTP_GET,  .handler = handler_redirect};
    httpd_uri_t cap_win7  = {.uri = "/ncsi.txt",               .method = HTTP_GET,  .handler = handler_redirect};
    httpd_uri_t cap_win10 = {.uri = "/connecttest.txt",        .method = HTTP_GET,  .handler = handler_redirect};
    httpd_uri_t wild      = {.uri = "/*",                      .method = HTTP_GET,  .handler = handler_redirect};

    httpd_register_uri_handler(*server, &root);
    httpd_register_uri_handler(*server, &save);
    httpd_register_uri_handler(*server, &cap_andr);
    httpd_register_uri_handler(*server, &cap_ios);
    httpd_register_uri_handler(*server, &cap_win7);
    httpd_register_uri_handler(*server, &cap_win10);
    httpd_register_uri_handler(*server, &wild);   /* registered last */
    return ESP_OK;
}

/* PM lock held for the duration of the provisioning window. Originally added
 * as an experiment against a suspected PM/tickless-idle race during SoftAP
 * STA-join — that hypothesis was disproven (the lock does not change the
 * crash, and `esp_pm_configure()` is not called in this project, so PM is
 * effectively inert). The lock stays in place because it is the right shape
 * if `esp_pm_configure()` is ever wired in, and removing it would only save
 * a few bytes of code. The lock is created lazily on first call and reused
 * across teardown/restart cycles. */
static esp_pm_lock_handle_t s_prov_pm_lock = NULL;

static void prov_pm_lock_acquire(void)
{
#if CONFIG_PM_ENABLE
    if (!s_prov_pm_lock) {
        esp_err_t err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP,
                                            0, "devdash_prov",
                                            &s_prov_pm_lock);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_pm_lock_create failed: %s", esp_err_to_name(err));
            s_prov_pm_lock = NULL;
            return;
        }
    }
    esp_pm_lock_acquire(s_prov_pm_lock);
#endif
}

static void prov_pm_lock_release(void)
{
#if CONFIG_PM_ENABLE
    if (s_prov_pm_lock) esp_pm_lock_release(s_prov_pm_lock);
#endif
}

static esp_err_t run_provisioning_window(void)
{
    xEventGroupClearBits(s_wifi_events, BIT_PROV_DONE);
    prov_pm_lock_acquire();

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) { prov_pm_lock_release(); return ESP_FAIL; }

    derive_ap_ssid(s_portal_ssid, sizeof(s_portal_ssid));
    if (storage_get_or_init_ap_password(s_portal_password,
                                        sizeof(s_portal_password)) != ESP_OK) {
        ESP_LOGE(TAG, "AP password load/init failed");
        prov_pm_lock_release();
        return ESP_FAIL;
    }

    esp_event_handler_instance_t ap_event_instance = NULL;
    esp_err_t err = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, provisioning_wifi_event, NULL,
        &ap_event_instance);
    if (err != ESP_OK) {
        esp_netif_destroy_default_wifi(ap_netif);
        prov_pm_lock_release();
        return err;
    }

    /* Provisioning does not need a station interface. AP-only mode avoids STA
     * channel/coexistence state influencing beaconing while the portal is up. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    wifi_config_t ap_config = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .ssid_hidden = 0,
            .beacon_interval = 100,
            .pairwise_cipher = WIFI_CIPHER_TYPE_CCMP,
        },
    };
    strncpy((char *)ap_config.ap.ssid, s_portal_ssid,
            sizeof(ap_config.ap.ssid) - 1);
    strncpy((char *)ap_config.ap.password, s_portal_password,
            sizeof(ap_config.ap.password) - 1);
    ap_config.ap.ssid_len = strlen(s_portal_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20));
    /* 11b/g only (no 11n) for the SoftAP: removes 11n/AMPDU from the AP path,
     * the documented workaround for ESP32-S3 SoftAPs that log as started but
     * are undetectable by phones/PCs (ESP-IDF issue #13508). */
    ESP_ERROR_CHECK(esp_wifi_set_protocol(
        WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G));
    ESP_ERROR_CHECK(esp_wifi_start());

    captive_dns_start();

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(start_portal_server(&server));

    /* Password intentionally not logged — it lands in serial buffers and would
     * leak the SoftAP PoP. Read it from the e-ink QR or the SETUP screen. */
    int8_t tx_power = 0;
    esp_wifi_get_max_tx_power(&tx_power);
    wifi_config_t active_config = {0};
    esp_wifi_get_config(WIFI_IF_AP, &active_config);
    ESP_LOGI(TAG,
             "Starting SoftAP HTTP provisioning portal: ssid=%s hidden=%u "
             "channel=%u beacon=%ums tx_power=%d quarter-dBm "
             "url=http://192.168.4.1",
             s_portal_ssid, active_config.ap.ssid_hidden,
             active_config.ap.channel, active_config.ap.beacon_interval,
             tx_power);

    /* Wait for the user to save credentials (BIT_PROV_DONE; a successful POST
     * schedules esp_restart()), honouring the configured timeout, while also
     * polling the BOOT button for the device-side reset gesture. No boot-button
     * monitor task runs during provisioning (it starts only on the STA path,
     * after this branch), so a long-press here is unambiguous.
     *
     * The gesture is two-tier, mirroring an erase-flash without the web flasher:
     *   1. A long-press forgets the saved Wi-Fi networks (display/panel settings
     *      are kept) and shows the confirm screen.
     *   2. A second BOOT press within 10 s escalates to a full factory reset —
     *      the entire NVS partition is erased on the next boot (armed via the
     *      RTC flag in boot_button, applied before nvs_flash_init in app_main).
     * If no second press arrives, only the Wi-Fi wipe takes effect on restart. */
    const TickType_t poll = pdMS_TO_TICKS(250);
    const int64_t deadline_us = CONFIG_DEVDASH_PROV_TIMEOUT_S > 0
        ? esp_timer_get_time() + (int64_t)CONFIG_DEVDASH_PROV_TIMEOUT_S * 1000000
        : 0; /* 0 = no deadline */
    EventBits_t bits = 0;
    for (;;) {
        bits = xEventGroupWaitBits(s_wifi_events, BIT_PROV_DONE,
                                   pdTRUE, pdFALSE, poll);
        if (bits & BIT_PROV_DONE) break;
        if (boot_button_is_pressed() &&
            boot_button_wait_longpress(CONFIG_DEVDASH_BOOT_LONGPRESS_MS)) {
            boot_button_wait_release();
            ESP_LOGW(TAG, "Setup-screen long-press: forgetting Wi-Fi networks");

            /* Tier 1: clear the saved Wi-Fi credentials, preserving the rest of
             * the config (panel variant, refresh interval, etc.). cfg_v2 is
             * ~7 KB — heap-allocate to keep it off this task's stack. Failures
             * are logged, not hidden: a full/failed NVS is exactly when Tier 2
             * (full erase) is the recovery, and the confirm screen already
             * points there ("BOOT again = WIPE ALL"), so fall through. */
            bool wiped = false;
            dash_config_v2_t *cfg = calloc(1, sizeof(*cfg));
            if (cfg) {
                storage_load_v2(cfg);
                cfg->network_count = 0;
                memset(cfg->networks, 0, sizeof(cfg->networks));
                esp_err_t serr = storage_save_v2(cfg);
                free(cfg);
                if (serr == ESP_OK) {
                    wiped = true;
                } else {
                    ESP_LOGE(TAG, "Wi-Fi wipe save failed: %s — press BOOT again "
                             "for a full factory erase", esp_err_to_name(serr));
                }
            } else {
                ESP_LOGE(TAG, "Wi-Fi wipe: cfg alloc failed — press BOOT again "
                         "for a full factory erase");
            }

            /* Tier 2: offer a full factory wipe. The screen reflects whether the
             * Tier-1 wipe committed (so a failure is not shown as success), and
             * always offers the full erase — the recovery when Tier 1 could not
             * persist. Watch for a second BOOT press within a short window. */
            display_show_factory_confirm(wiped);
            const int64_t confirm_deadline =
                esp_timer_get_time() + 10 * 1000000; /* 10 s */
            bool factory = false;
            while (esp_timer_get_time() < confirm_deadline) {
                if (boot_button_is_pressed()) {
                    boot_button_wait_release();
                    factory = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (factory) {
                ESP_LOGW(TAG, "Factory reset confirmed via BOOT press");
                boot_button_request_factory_reset();
            }
            esp_restart();
        }
        if (deadline_us != 0 && esp_timer_get_time() >= deadline_us) break;
    }
    if (server) httpd_stop(server);
    captive_dns_stop();
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (ap_netif) esp_netif_destroy_default_wifi(ap_netif);
    if (ap_event_instance) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              ap_event_instance);
    }
    prov_pm_lock_release();

    return (bits & BIT_PROV_DONE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_net_open_config_window(void)
{
    return run_provisioning_window();
}

esp_err_t wifi_net_provision_if_needed(void)
{
    if (wifi_net_is_provisioned()) return ESP_OK;
    return run_provisioning_window();
}

void wifi_net_stop(void)
{
    esp_wifi_disconnect();
    esp_wifi_stop();
}
