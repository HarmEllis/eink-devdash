# Board Notes — ESP32-S3 Super Mini (nologo.tech)

Hardware-specific quirks and verified behavior for the exact module in use.

## Wake source: GPIO0 (BOOT button)

**Verified on:** 2026-05-17 with `firmware/main/main_bringup.c`.

The BOOT button on the Super Mini is wired to **`GPIO_NUM_0`** and is the
only physical button available for waking the chip from deep sleep (the
RESET button performs a hard reset, which is observable as
`ESP_SLEEP_WAKEUP_UNDEFINED` and clears RTC memory).

`GPIO0` is also a strapping pin: it must be high at reset. The internal
pull-up keeps it high in active mode, and the BOOT button pulls it to GND
when pressed.

### Configuration that works

Two pull-up steps are required because they live in different power
domains on the ESP32-S3:

```c
/* Active mode — digital IO domain, used while the chip is awake. */
gpio_config_t io = {
    .pin_bit_mask = 1ULL << GPIO_NUM_0,
    .mode         = GPIO_MODE_INPUT,
    .pull_up_en   = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_DISABLE,
};
gpio_config(&io);

/* Deep-sleep — RTC IO domain, used while the digital IO domain is off. */
rtc_gpio_init(GPIO_NUM_0);
rtc_gpio_set_direction(GPIO_NUM_0, RTC_GPIO_MODE_INPUT_ONLY);
rtc_gpio_pullup_en(GPIO_NUM_0);
rtc_gpio_pulldown_dis(GPIO_NUM_0);

esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);   /* wake on level 0 */
esp_sleep_enable_timer_wakeup(timer_us);       /* fallback timer  */
```

**Why both pulls:** during deep sleep the digital IO domain is powered
off, so a pull-up set via `gpio_config` is gone. The pin floats, ext0
fires immediately (level 0), and the chip enters a wake/sleep boot loop.
Configuring the pull-up through `rtc_gpio_*` keeps it asserted by the
RTC domain, which stays powered.

### Observed behavior

| Observation | Result |
|-------------|--------|
| Idle GPIO0 level in active mode | `1` (pull-up holds it high) |
| Button-press level | `0`, clean transition, no visible bounce |
| BOOT pressed during deep sleep | Chip wakes within ~1 s, app_main re-runs |
| No input during deep sleep | Chip stays asleep until the timer fallback fires (confirmed indirectly: no spurious wakes / boot loop) |

### Pins used elsewhere (do not reuse for wake)

- `GPIO9` — e-ink D/C
- `GPIO10` — e-ink CS
- `GPIO11` — SPI MOSI
- `GPIO12` — SPI SCK
- `GPIO13` — e-ink BUSY
- `GPIO1`  — e-ink RST
- `GPIO48` — onboard LED

## USB-Serial-JTAG behavior

The Super Mini has no external USB-UART bridge; it uses the ESP32-S3's
built-in USB-Serial-JTAG. Implications:

- Improv and browser Web Serial traffic must use USB-Serial-JTAG, not
  UART0. UART0 is only useful if a separate wired debug adapter is attached.
- `esp_deep_sleep_start()` powers down the USB-CDC controller. The host
  COM port disappears for the entire sleep duration.
- After wake, the host needs ~0.5-1.5 s to re-enumerate the USB device,
  and any logs written during that window are silently dropped.
- Browser-based Web Serial monitors (e.g. the flash-server's terminal)
  require a manual "Select port" click to reattach after re-enumeration
  — they will not reconnect automatically.
- Opening the USB-Serial-JTAG CDC port for plain serial I/O should not reset
  the chip by itself. A boot log line such as
  `rst:0x15 (USB_UART_CHIP_RESET)` means the host sent the USB-Serial-JTAG
  reset control sequence, normally via DTR/RTS transitions used by esptool,
  ESP Web Tools, or a terminal that asserts modem-control lines on open.
  ESP-IDF v5.3 has `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION`, but that only
  prevents automatic light sleep while the USB-Serial-JTAG port is connected;
  it does not disable the reset/download control sequence.

For development this is workable but not pleasant; for production it is
irrelevant because the chip is battery-powered and the host is absent.
When iterating on sleep-related code, either:

1. Use a serial monitor that auto-reconnects (`idf.py monitor`,
   `screen /dev/ttyACM0 115200`, `tio`, etc.) on a Linux/macOS host
   that has direct `/dev/ttyACM*` access, or
2. Persist diagnostic state in RTC slow memory (`RTC_DATA_ATTR`) so the
   next reachable boot can re-emit the missed history. The bring-up
   firmware used this technique.

## Cold boot vs reset vs wake (RTC memory semantics)

- **Cold boot** (power applied): all RTC memory zero-initialised.
  `esp_sleep_get_wakeup_cause()` returns `UNDEFINED`.
- **RESET button**: same as cold boot for RTC memory — values reset.
  `esp_sleep_get_wakeup_cause()` returns `UNDEFINED`.
- **Wake from deep sleep**: RTC slow memory (`RTC_DATA_ATTR`) is
  preserved; `RTC_NOINIT_ATTR` is preserved without zero-init. The wake
  cause reflects the source (`EXT0`, `TIMER`, etc.).

## V4 provisioning contract (SoftAP HTTP portal)

The on-device captive portal — served from `wifi_prov.c` at
`http://192.168.4.1/` while the AP is up — pre-renders all 5 networks × 5
APIs slots so the form is usable with JavaScript disabled. The C handler
parses `application/x-www-form-urlencoded` body fields by positional
prefix. Field names match `design_handoff_eink_v4_provisioning/README.md`:

| Field            | Type     | Meaning                                          |
|------------------|----------|--------------------------------------------------|
| `wN_i`           | hidden   | Profile id of network N (0 for empty slots).     |
| `wN_on`          | checkbox | Network N enabled. Off ⇒ remove on save.         |
| `wN_ssid`        | text     | SSID (1..32 chars).                              |
| `wN_pass`        | password | New password. Empty + clearpw off ⇒ keep saved.  |
| `wN_clearpw`     | checkbox | Force-erase saved password.                      |
| `wN_aK_i`        | hidden   | Profile id of API K under network N.             |
| `wN_aK_on`       | checkbox | API K enabled.                                   |
| `wN_aK_url`      | url      | `http://` only — TLS is out of scope.            |
| `wN_aK_tok`      | password | New token. Same keep/clear/replace rules as PASS.|
| `wN_aK_cleartok` | checkbox | Force-erase saved token.                         |
| `iv`             | number   | Refresh interval, 3..60 (stored in cfg.refresh_min). |

`N ∈ 0..4`, `K ∈ 0..4`. Validation lives in `validate_network` and mirrors
`storage_validate_api_url`. Body cap is 24 KB — anything larger gets 413.
`POST /save` returns the V4 saved page and schedules `esp_restart()` 4 s
later via `esp_timer`, leaving time for the on-page spinner→check
transition before the AP drops.

The HTTP server runs with `uri_match_fn = httpd_uri_match_wildcard`. The
explicit captive-detect routes (`/generate_204`, `/hotspot-detect.html`,
`/ncsi.txt`, `/connecttest.txt`) and the catch-all `/*` reply 302 to
`http://192.168.4.1/`. The wildcard must be registered last so the more
specific URIs match first.

The captive DNS responder (`captive_dns.c`) binds UDP/53 to the AP netif
only and answers every A query with `192.168.4.1`. AAAA / HTTPS / SVCB /
unknown types get NOERROR + ANCOUNT=0 — never a timeout — so iOS and
Android pop the captive sheet promptly.

The on-screen QR encodes the standard WiFi join string
(`WIFI:T:WPA;S:devdash-XXXX;P:<12-char alnum>;;`). The AP password is
generated once at first boot via `storage_get_or_init_ap_password` and
persisted under NVS key `ap_pwd` in the `devdash` namespace. Factory
reset (`storage_erase`) clears the key; the next boot generates a new
password. ADR-0002 records the policy.
