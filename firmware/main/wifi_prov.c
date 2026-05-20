#include "wifi_prov.h"
#include "captive_dns.h"
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
    api_form_t apis[MAX_APIS_PER_NETWORK];
} net_form_t;

typedef struct {
    net_form_t nets[MAX_WIFI_NETWORKS];
    int        iv;
    bool       iv_present;
} portal_form_t;

/* Connection is driven explicitly by wifi_roam_connect (esp_wifi_connect on
 * the SSID we pick after scanning) and by Improv when the optional USB
 * provisioning path is used.
 * We intentionally do NOT auto-connect on STA_START or auto-retry on
 * STA_DISCONNECTED here: doing so kicks off a connect with whatever credentials
 * are still in WiFi NVS and blocks the scan path with
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

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
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
        char *end = amp ? amp : body + strlen(body);
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
"main{padding:16px;max-width:560px;margin:0 auto}"
"section.block{margin:0 0 20px}"
"section.block>h2{font-size:12px;font-weight:600;text-transform:uppercase;letter-spacing:0.06em;"
"color:#6b7280;margin:0 0 8px;padding:0 4px}"
".blockhint{font-size:12px;color:#6b7280;margin:-4px 4px 8px}"
".card{background:#fff;border:1px solid #e3e5e8;border-radius:12px;padding:14px 14px 6px;"
"margin-bottom:10px}"
/* JS-driven collapse: when a slot is toggled off, JS adds `.disabled`. */
".card.disabled .body{display:none}"
/* Pure-CSS fallback for browsers with :has() support — keeps the form usable
 * without JS. The body collapses when its own toggle is unchecked. */
".card:has(>.row.head input[type=checkbox][name$=\"_on\"]:not(:checked)) .body{display:none}"
".api-list>li:has(>.row.head input[type=checkbox][name$=\"_on\"]:not(:checked)) .body{display:none}"
".card .row{display:flex;align-items:center;gap:10px;padding:4px 0;min-height:32px}"
".card .row.head{border-bottom:1px solid #eef0f3;padding-bottom:10px;margin-bottom:8px}"
".card .pill{font-size:11px;font-weight:600;letter-spacing:0.04em;text-transform:uppercase;"
"color:#6b7280;background:#f1f2f4;padding:3px 8px;border-radius:99px}"
".card .pill.saved{color:#2a4a7a;background:#e6eef9}"
".card .pill.new{color:#7a4a2a;background:#fbeede}"
".card .grow{flex:1 1 auto;min-width:0}"
"label.field{display:block;margin:8px 0}"
"label.field>.lab{display:block;font-size:12px;color:#4b5563;margin-bottom:4px}"
"input[type=text],input[type=password],input[type=number],input[type=url]{"
"width:100%;min-height:44px;padding:10px 12px;font:inherit;font-size:15px;color:#1c1f24;"
"background:#fff;border:1px solid #cbd0d6;border-radius:8px;outline:none}"
"input:focus{border-color:#2563eb;box-shadow:0 0 0 3px rgba(37,99,235,.15)}"
"input.invalid{border-color:#c83737;background:#fff5f5}"
".err{color:#a02020;font-size:12px;margin-top:4px}"
".toggle{display:inline-flex;align-items:center;gap:8px;cursor:pointer;user-select:none}"
".toggle input{position:absolute;opacity:0;pointer-events:none}"
".toggle .track{display:inline-block;width:34px;height:20px;background:#cbd0d6;"
"border-radius:99px;position:relative;transition:background .15s}"
".toggle .track::after{content:\"\";position:absolute;top:2px;left:2px;width:16px;height:16px;"
"background:#fff;border-radius:50%;transition:left .15s;box-shadow:0 1px 2px rgba(0,0,0,.2)}"
".toggle input:checked+.track{background:#2563eb}"
".toggle input:checked+.track::after{left:16px}"
".secret{display:flex;gap:8px;align-items:stretch}"
".secret input{flex:1}"
".secret .showbtn{background:#f1f2f4;border:1px solid #cbd0d6;border-radius:8px;"
"padding:0 12px;font:inherit;font-size:13px;cursor:pointer;color:#1c1f24}"
".check{display:flex;align-items:center;gap:8px;margin-top:6px;font-size:13px;color:#4b5563}"
".api-list{list-style:none;margin:8px 0;padding:0}"
".api-list>li{background:#fafbfc;border:1px dashed #cbd0d6;border-radius:10px;"
"padding:10px 12px;margin-bottom:8px}"
".api-list>li.disabled .body{display:none}"
".interval-grid{display:grid;grid-template-columns:1fr 80px;gap:12px;align-items:center;"
"background:#fff;border:1px solid #e3e5e8;border-radius:12px;padding:12px}"
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
"body.saved>main,body.saved>header.topbar,body.saved>.savebar{display:none}"
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
"document.addEventListener('change',function(e){"
"var t=e.target;"
"if(t.matches('input[name$=\"_on\"][type=checkbox]')){"
"if(t.name.indexOf('_a')>0)tgl(t,'.api-list>li','disabled');"
"else tgl(t,'.card','disabled');"
"}"
"});"
"document.addEventListener('click',function(e){"
"if(e.target.matches('.showbtn')){"
"var inp=e.target.parentElement.querySelector('input');"
"inp.type=inp.type==='password'?'text':'password';"
"e.target.textContent=inp.type==='password'?'show':'hide';"
"}"
"});"
"function syncRange(a,b){a.addEventListener('input',function(){b.value=a.value});"
"b.addEventListener('input',function(){a.value=b.value});}"
"window.addEventListener('DOMContentLoaded',function(){"
"var r=document.getElementById('iv-range');var n=document.getElementById('iv-num');"
"if(r&&n)syncRange(r,n);"
"document.querySelectorAll('input[name$=\"_on\"][type=checkbox]').forEach(function(t){"
"if(t.name.indexOf('_a')>0)tgl(t,'.api-list>li','disabled');"
"else tgl(t,'.card','disabled');"
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
    char url_esc[DASH_API_URL_MAX * 6 + 1];
    char buf[DASH_API_URL_MAX * 6 + 512];

    /* No initial `disabled` class. With JS, V4_JS adds it on load. Without
     * JS, browsers with :has() honour the pure-CSS rule above; older
     * browsers just show the body — the user can still toggle and edit. */
    snprintf(buf, sizeof(buf),
        "<li data-api=\"%d-%d\">"
        "<div class=\"row head\">"
        "<label class=\"toggle\">"
        "<input type=\"checkbox\" name=\"w%d_a%d_on\"%s>"
        "<span class=\"track\"></span></label>"
        "<span class=\"pill %s\">%s</span>"
        "<input type=\"hidden\" name=\"w%d_a%d_i\" value=\"%lu\">"
        "</div><div class=\"body\">",
        n, k,
        n, k, en ? " checked" : "",
        (api && api->id) ? "saved" : "new",
        (api && api->id) ? "saved" : "new",
        n, k, (unsigned long)(api ? api->id : 0));
    CHUNK(req, buf);

    html_escape(api && api->api_url[0] ? api->api_url : "", url_esc, sizeof(url_esc));
    snprintf(buf, sizeof(buf),
        "<label class=\"field\"><span class=\"lab\">API URL</span>"
        "<input type=\"url\" name=\"w%d_a%d_url\" maxlength=\"191\" "
        "placeholder=\"http://192.168.1.50:3000\" value=\"%s\">"
        "</label>", n, k, url_esc);
    CHUNK(req, buf);

    bool has_tok = api && api->device_token[0];
    snprintf(buf, sizeof(buf),
        "<label class=\"field\"><span class=\"lab\">Device token</span>"
        "<div class=\"secret\">"
        "<input type=\"password\" name=\"w%d_a%d_tok\" maxlength=\"64\" "
        "placeholder=\"%s\" autocomplete=\"new-password\">"
        "<button type=\"button\" class=\"showbtn\">show</button></div>"
        "<label class=\"check\"><input type=\"checkbox\" name=\"w%d_a%d_cleartok\">"
        "Clear saved token</label></label>",
        n, k, has_tok ? "current token kept" : "device token",
        n, k);
    CHUNK(req, buf);

    CHUNK(req, "</div></li>");
}

/* Render one WiFi network card and its 5 API slots. */
static void render_network(httpd_req_t *req, int n,
                           const dash_wifi_profile_t *net)
{
    bool en = net && net->enabled;
    char ssid_esc[DASH_SSID_MAX * 6 + 1];
    char buf[DASH_SSID_MAX * 6 + 512];
    snprintf(buf, sizeof(buf),
        "<article class=\"card\" data-net=\"%d\">"
        "<div class=\"row head\">"
        "<label class=\"toggle\">"
        "<input type=\"checkbox\" name=\"w%d_on\"%s>"
        "<span class=\"track\"></span></label>"
        "<span class=\"pill %s\">%s</span>"
        "<span class=\"grow\"></span>"
        "<input type=\"hidden\" name=\"w%d_i\" value=\"%lu\">"
        "</div><div class=\"body\">",
        n,
        n, en ? " checked" : "",
        (net && net->id) ? "saved" : "new",
        (net && net->id) ? "saved" : "new",
        n, (unsigned long)(net ? net->id : 0));
    CHUNK(req, buf);

    html_escape(net && net->ssid[0] ? net->ssid : "", ssid_esc, sizeof(ssid_esc));
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
        "<label class=\"check\"><input type=\"checkbox\" name=\"w%d_clearpw\">"
        "Clear saved password</label></label>",
        n, has_pwd ? "current password kept" : "WPA2 password",
        n);
    CHUNK(req, buf);

    CHUNK(req, "<ul class=\"api-list\">");
    for (int k = 0; k < MAX_APIS_PER_NETWORK; k++) {
        const dash_api_profile_t *api =
            (net && k < net->api_count) ? &net->apis[k] : NULL;
        render_api(req, n, k, api);
    }
    CHUNK(req, "</ul></div></article>");
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

    CHUNK(req,
        "<main><form id=\"cfg\" method=\"post\" action=\"/save\" novalidate>"
        "<section class=\"block\" id=\"wifi-block\">"
        "<h2>WiFi networks</h2>"
        "<p class=\"blockhint\">Toggle a slot to add a network; toggle off "
        "to remove it on save. Up to 5 networks &times; 5 APIs each.</p>");

    for (int n = 0; n < MAX_WIFI_NETWORKS; n++) {
        const dash_wifi_profile_t *net =
            (n < cfg->network_count) ? &cfg->networks[n] : NULL;
        render_network(req, n, net);
    }

    char iv_buf[1024];
    int iv = cfg->refresh_min ? cfg->refresh_min : 5;
    snprintf(iv_buf, sizeof(iv_buf),
        "</section>"
        "<section class=\"block\" id=\"display-block\">"
        "<h2>Display</h2>"
        "<div class=\"interval-grid\">"
        "<label class=\"field\" style=\"margin:0\"><span class=\"lab\">"
        "Refresh interval (min)</span>"
        "<input type=\"range\" id=\"iv-range\" min=\"3\" max=\"60\" value=\"%d\">"
        "</label>"
        "<label class=\"field\" style=\"margin:0\"><span class=\"lab\">Minutes</span>"
        "<input type=\"number\" id=\"iv-num\" name=\"iv\" min=\"3\" max=\"60\" value=\"%d\">"
        "</label></div>"
        "<p class=\"blockhint\">Red-bearing refreshes take ~15s; pick a higher "
        "interval if you don't need fresh data each cycle.</p>"
        "</section>",
        iv, iv);
    CHUNK(req, iv_buf);

    CHUNK(req,
        "<div class=\"savebar\"><div class=\"inner\">"
        "<div class=\"summary\">Fix the highlighted fields before saving.</div>"
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

    if (form->iv_present && form->iv >= 3 && form->iv <= 60) {
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
        out_n++;
        if (out_n >= MAX_WIFI_NETWORKS) break;
    }
    cfg->network_count = out_n;
    return ESP_OK;
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
    if (form->iv_present && (form->iv < 3 || form->iv > 60)) {
        reason = "Refresh interval must be between 3 and 60.";
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
        }
        for (int k = 0; k < MAX_APIS_PER_NETWORK && !reason; k++) {
            const api_form_t *af = &nf->apis[k];
            if (!af->enabled) continue;
            if (af->overflow) {
                reason = "An API URL or token is longer than this device accepts.";
            } else if (!storage_validate_api_url(af->url)) {
                reason = "Each enabled API needs a valid http:// URL.";
            } else if (af->id == 0 && !af->tok_present) {
                reason = "Each new API entry needs a device token.";
            }
        }
    }
    if (reason) {
        free(form);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_sendstr(req, reason);
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
    esp_err_t err = storage_save_v2(cfg);
    free(form); free(prev); free(cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage_save_v2 failed: %d", err);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Save failed.");
        return ESP_OK;
    }

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

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_config_t ap_config = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)ap_config.ap.ssid, s_portal_ssid,
            sizeof(ap_config.ap.ssid) - 1);
    strncpy((char *)ap_config.ap.password, s_portal_password,
            sizeof(ap_config.ap.password) - 1);
    ap_config.ap.ssid_len = strlen(s_portal_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    captive_dns_start();

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(start_portal_server(&server));

    /* Password intentionally not logged — it lands in serial buffers and would
     * leak the SoftAP PoP. Read it from the e-ink QR or the SETUP screen. */
    ESP_LOGI(TAG, "Starting SoftAP HTTP provisioning portal: ssid=%s url=http://192.168.4.1",
             s_portal_ssid);

    /* Wait up to the configured timeout for the user to save credentials
     * via the HTTP portal (POST /save sets BIT_PROV_DONE before the deferred
     * restart fires). */
    TickType_t ticks = CONFIG_DEVDASH_PROV_TIMEOUT_S > 0
        ? pdMS_TO_TICKS(CONFIG_DEVDASH_PROV_TIMEOUT_S * 1000)
        : portMAX_DELAY;
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, BIT_PROV_DONE,
                                           pdTRUE, pdFALSE, ticks);
    if (server) httpd_stop(server);
    captive_dns_stop();
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (ap_netif) esp_netif_destroy_default_wifi(ap_netif);
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
