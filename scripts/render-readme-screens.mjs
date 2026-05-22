#!/usr/bin/env node

import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const root = join(__dirname, "..");
const displaySource = readFileSync(join(root, "firmware/main/display.c"), "utf8");

const W = 296;
const H = 128;
const FONT_W = 6;
const FONT_BOOT_W = 6;
const FONT2_W = 12;

function parseFont(source) {
  const match = source.match(/static const uint8_t font5x7\[\]\[5\] = \{([\s\S]*?)\n\};/);
  if (!match) throw new Error("font5x7 table not found in firmware/main/display.c");

  const glyphs = [];
  const glyphPattern = /\{\s*(0x[0-9A-Fa-f]{2})\s*,\s*(0x[0-9A-Fa-f]{2})\s*,\s*(0x[0-9A-Fa-f]{2})\s*,\s*(0x[0-9A-Fa-f]{2})\s*,\s*(0x[0-9A-Fa-f]{2})\s*\}/g;
  for (const glyph of match[1].matchAll(glyphPattern)) {
    glyphs.push(glyph.slice(1).map((v) => Number.parseInt(v, 16)));
  }
  if (glyphs.length !== 91) {
    throw new Error(`expected 91 glyphs, found ${glyphs.length}`);
  }
  return glyphs;
}

const font5x7 = parseFont(displaySource);

class Frame {
  constructor() {
    this.bw = Array.from({ length: H }, () => Array(W).fill(1));
    this.red = Array.from({ length: H }, () => Array(W).fill(0));
  }

  lpix(x, y, black, useRed) {
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    if (useRed) {
      this.red[y][x] = black ? 1 : 0;
    } else {
      this.bw[y][x] = black ? 0 : 1;
    }
  }

  fillRect(x, y, w, h, black, useRed) {
    for (let dy = 0; dy < h; dy++) {
      for (let dx = 0; dx < w; dx++) {
        this.lpix(x + dx, y + dy, black, useRed);
      }
    }
  }

  hline(x, y, len) {
    for (let i = 0; i < len; i++) this.lpix(x + i, y, 1, 0);
  }

  vline(x, y, len) {
    for (let i = 0; i < len; i++) this.lpix(x, y + i, 1, 0);
  }

  drawChar(x, y, ch, useRed) {
    const code = ch.charCodeAt(0);
    const safe = code < 32 || code > 122 ? "?".charCodeAt(0) : code;
    const glyph = font5x7[safe - 32];
    for (let col = 0; col < 5; col++) {
      for (let row = 0; row < 7; row++) {
        if (glyph[col] & (1 << row)) this.lpix(x + col, y + row, 1, useRed);
      }
    }
  }

  drawCharBw(x, y, ch, black) {
    const code = ch.charCodeAt(0);
    const safe = code < 32 || code > 122 ? "?".charCodeAt(0) : code;
    const glyph = font5x7[safe - 32];
    for (let col = 0; col < 5; col++) {
      for (let row = 0; row < 7; row++) {
        if (glyph[col] & (1 << row)) this.lpix(x + col, y + row, black, 0);
      }
    }
  }

  drawStr(x, y, text, useRed) {
    for (const ch of text) {
      this.drawChar(x, y, ch, useRed);
      x += FONT_W;
    }
  }

  drawStrAdv(x, y, text, black, advance) {
    for (const ch of text) {
      this.drawCharBw(x, y, ch, black);
      x += advance;
    }
  }

  drawStrClippedAdv(x, y, text, black, advance, maxW) {
    let used = 0;
    for (const ch of text) {
      if (used + 5 > maxW) break;
      this.drawCharBw(x, y, ch, black);
      x += advance;
      used += advance;
    }
  }

  drawChar2x(x, y, ch, useRed) {
    const code = ch.charCodeAt(0);
    const safe = code < 32 || code > 122 ? "?".charCodeAt(0) : code;
    const glyph = font5x7[safe - 32];
    for (let col = 0; col < 5; col++) {
      for (let row = 0; row < 7; row++) {
        if (glyph[col] & (1 << row)) this.fillRect(x + col * 2, y + row * 2, 2, 2, 1, useRed);
      }
    }
  }

  drawStr2x(x, y, text, useRed) {
    for (const ch of text) {
      this.drawChar2x(x, y, ch, useRed);
      x += FONT2_W;
    }
  }

  drawChar4xBw(x, y, ch, black) {
    const code = ch.charCodeAt(0);
    const safe = code < 32 || code > 122 ? "?".charCodeAt(0) : code;
    const glyph = font5x7[safe - 32];
    for (let col = 0; col < 5; col++) {
      for (let row = 0; row < 7; row++) {
        if (glyph[col] & (1 << row)) this.fillRect(x + col * 4, y + row * 4, 4, 4, black, 0);
      }
    }
  }

  drawStr4xBw(x, y, text, black) {
    for (const ch of text) {
      this.drawChar4xBw(x, y, ch, black);
      x += 23;
    }
  }

  toSvg(title) {
    const blackRuns = [];
    const redRuns = [];
    for (let y = 0; y < H; y++) {
      let x = 0;
      while (x < W) {
        const color = this.red[y][x] ? "red" : this.bw[y][x] ? "white" : "black";
        if (color === "white") {
          x++;
          continue;
        }
        const start = x;
        while (x < W) {
          const next = this.red[y][x] ? "red" : this.bw[y][x] ? "white" : "black";
          if (next !== color) break;
          x++;
        }
        const run = `M${start} ${y}h${x - start}v1H${start}z`;
        if (color === "red") redRuns.push(run);
        else blackRuns.push(run);
      }
    }

    return [
      `<svg xmlns="http://www.w3.org/2000/svg" width="${W * 3}" height="${H * 3}" viewBox="0 0 ${W} ${H}" role="img" aria-labelledby="title">`,
      `<title id="title">${escapeXml(title)}</title>`,
      `<rect width="${W}" height="${H}" fill="#fff"/>`,
      `<g shape-rendering="crispEdges">`,
      blackRuns.length ? `<path fill="#111" d="${blackRuns.join("")}"/>` : "",
      redRuns.length ? `<path fill="#d62518" d="${redRuns.join("")}"/>` : "",
      `</g>`,
      `</svg>`,
      "",
    ].join("\n");
  }
}

function strW(text) {
  return text.length * FONT_W;
}

function strWAdv(text, advance) {
  return text.length * advance;
}

function str2xW(text) {
  return text.length * FONT2_W;
}

function iconBoxLogo(f, ox, oy) {
  f.fillRect(ox, oy, 9, 9, 1, 0);
  f.fillRect(ox + 2, oy + 2, 5, 5, 0, 0);
  f.fillRect(ox + 3, oy + 3, 3, 3, 1, 0);
}

function iconIssue(f, ox, oy, useRed) {
  f.fillRect(ox + 3, oy + 0, 4, 1, 1, useRed);
  f.fillRect(ox + 2, oy + 1, 6, 1, 1, useRed);
  f.fillRect(ox + 1, oy + 2, 2, 6, 1, useRed);
  f.fillRect(ox + 7, oy + 2, 2, 6, 1, useRed);
  f.fillRect(ox + 2, oy + 8, 6, 1, 1, useRed);
  f.fillRect(ox + 3, oy + 9, 4, 1, 1, useRed);
  f.fillRect(ox + 4, oy + 4, 2, 2, 1, useRed);
}

function iconPr(f, ox, oy, useRed) {
  f.fillRect(ox + 1, oy + 0, 2, 2, 1, useRed);
  f.fillRect(ox + 1, oy + 8, 2, 2, 1, useRed);
  f.fillRect(ox + 7, oy + 0, 2, 2, 1, useRed);
  f.fillRect(ox + 1, oy + 2, 1, 6, 1, useRed);
  f.fillRect(ox + 8, oy + 2, 1, 3, 1, useRed);
  f.fillRect(ox + 4, oy + 5, 4, 1, 1, useRed);
  f.fillRect(ox + 4, oy + 4, 1, 2, 1, useRed);
}

function iconShield(f, ox, oy, useRed) {
  f.fillRect(ox + 2, oy + 0, 6, 1, 1, useRed);
  f.fillRect(ox + 1, oy + 1, 8, 5, 1, useRed);
  f.fillRect(ox + 2, oy + 6, 6, 1, 1, useRed);
  f.fillRect(ox + 3, oy + 7, 4, 1, 1, useRed);
  f.fillRect(ox + 4, oy + 8, 2, 1, 1, useRed);
  if (useRed) {
    f.fillRect(ox + 4, oy + 2, 2, 2, 0, 0);
    f.fillRect(ox + 4, oy + 2, 2, 2, 0, 1);
    f.fillRect(ox + 4, oy + 5, 2, 1, 0, 0);
    f.fillRect(ox + 4, oy + 5, 2, 1, 0, 1);
  }
}

function iconKey(f, ox, oy, useRed) {
  f.fillRect(ox + 0, oy + 3, 3, 3, 1, useRed);
  f.fillRect(ox + 1, oy + 4, 1, 1, 0, 0);
  if (useRed) f.fillRect(ox + 1, oy + 4, 1, 1, 0, 1);
  f.fillRect(ox + 3, oy + 4, 6, 1, 1, useRed);
  f.fillRect(ox + 6, oy + 5, 1, 2, 1, useRed);
  f.fillRect(ox + 8, oy + 5, 1, 2, 1, useRed);
}

function iconHourglass(f, ox, oy, useRed) {
  f.fillRect(ox + 0, oy + 0, 7, 1, 1, useRed);
  f.fillRect(ox + 1, oy + 1, 5, 1, 1, useRed);
  f.fillRect(ox + 2, oy + 2, 3, 1, 1, useRed);
  f.fillRect(ox + 3, oy + 3, 1, 1, 1, useRed);
  f.fillRect(ox + 2, oy + 4, 3, 1, 1, useRed);
  f.fillRect(ox + 1, oy + 5, 5, 1, 1, useRed);
  f.fillRect(ox + 0, oy + 6, 7, 1, 1, useRed);
}

function iconSync(f, ox, oy) {
  f.fillRect(ox + 2, oy + 1, 5, 1, 1, 0);
  f.fillRect(ox + 1, oy + 2, 1, 3, 1, 0);
  f.fillRect(ox + 6, oy + 0, 1, 3, 1, 0);
  f.fillRect(ox + 2, oy + 7, 5, 1, 1, 0);
  f.fillRect(ox + 7, oy + 4, 1, 3, 1, 0);
  f.fillRect(ox + 2, oy + 6, 1, 3, 1, 0);
}

function iconWifi(f, ox, oy) {
  f.fillRect(ox + 2, oy + 2, 6, 1, 1, 0);
  f.fillRect(ox + 1, oy + 3, 1, 1, 1, 0);
  f.fillRect(ox + 8, oy + 3, 1, 1, 1, 0);
  f.fillRect(ox + 2, oy + 5, 1, 1, 1, 0);
  f.fillRect(ox + 3, oy + 5, 4, 1, 1, 0);
  f.fillRect(ox + 7, oy + 5, 1, 1, 1, 0);
  f.fillRect(ox + 4, oy + 7, 2, 1, 1, 0);
  f.fillRect(ox + 4, oy + 9, 2, 1, 1, 0);
}

function iconArrowRight(f, ox, oy) {
  f.hline(ox, oy + 3, 8);
  f.lpix(ox + 6, oy + 1, 1, 0);
  f.lpix(ox + 7, oy + 2, 1, 0);
  f.lpix(ox + 8, oy + 3, 1, 0);
  f.lpix(ox + 7, oy + 4, 1, 0);
  f.lpix(ox + 6, oy + 5, 1, 0);
}

function drawHeaderConnectionSlots(f, wifiSlot, apiSlot) {
  const iconW = 10;
  const arrowW = 9;
  const gap = 3;
  const wifi = String(wifiSlot);
  const api = String(apiSlot);
  let width = iconW + gap + strW(wifi);
  if (apiSlot) width += gap + arrowW + gap + strW(api);

  let x = (296 - width) / 2;
  iconWifi(f, x, 3);
  x += iconW + gap;
  f.drawStr(x, 5, wifi, 0);
  if (!apiSlot) return;
  x += strW(wifi) + gap;
  iconArrowRight(f, x, 5);
  x += arrowW + gap;
  f.drawStr(x, 5, api, 0);
}

function drawBarCfg(f, ox, oy, width, height, segW, pct) {
  const stride = segW + 1;
  const cols = Math.floor((width + 1) / stride);
  const filled = Math.floor((pct * cols + 50) / 100);
  const thresh = Math.floor((80 * cols + 50) / 100);
  for (let i = 0; i < cols; i++) {
    const sx = ox + i * stride;
    if (i < filled) {
      const isRed = pct > 80 && i >= thresh;
      f.fillRect(sx, oy, segW, height, 1, isRed);
    } else {
      f.hline(sx, oy, segW);
      f.hline(sx, oy + height - 1, segW);
      f.vline(sx, oy + 1, height - 2);
      f.vline(sx + segW - 1, oy + 1, height - 2);
    }
  }
}

function formatResetCountdown(seconds) {
  if (seconds <= 0) return "--";
  const totalMin = Math.floor(seconds / 60);
  if (totalMin < 60) return `0h${String(totalMin).padStart(2, "0")}`;
  if (totalMin < 24 * 60) {
    const h = Math.floor(totalMin / 60);
    const m = totalMin % 60;
    return `${h}h${String(m).padStart(2, "0")}`;
  }
  const totalHours = Math.floor(totalMin / 60);
  const d = Math.floor(totalHours / 24);
  const h = totalHours % 24;
  if (d > 9) return "10+d";
  return h < 10 ? `${d}d ${h}h` : `${d}d${h}h`;
}

function drawProvider(f, ox, oy, width, layout, title, ses, wk, sesResetS, wkResetS) {
  f.drawStr(ox, oy, title, 0);

  const sesS = formatResetCountdown(sesResetS);
  const wkS = formatResetCountdown(wkResetS);
  const textGap = 2;
  const hgGap = 4;
  const hgW = 7;
  const right = ox + width - 2;
  const xWk = right - strW(wkS);
  const xSlash = xWk - textGap - strW("/");
  const xSes = xSlash - textGap - strW(sesS);
  const xHg = xSes - hgGap - hgW;
  iconHourglass(f, xHg, oy, 0);
  f.drawStr(xSes, oy, sesS, ses > 80);
  f.drawStr(xSlash, oy, "/", 0);
  f.drawStr(xWk, oy, wkS, wk > 80);

  const barX = ox + layout.rowLabelW;
  const barW = width - layout.rowLabelW - layout.pctW;
  const sesY = oy + layout.titleRowH + layout.rowGap;
  const wkY = sesY + layout.barRowH + 2;
  const barDy = Math.floor((layout.barRowH - layout.barH) / 2);

  f.drawStr(ox, sesY + 2, "5H", 0);
  drawBarCfg(f, barX, sesY + barDy, barW, layout.barH, layout.segW, ses);
  const sesPct = `${ses}%`;
  f.drawStr(ox + width - strW(sesPct) - 2, sesY + 2, sesPct, ses > 80);

  f.drawStr(ox, wkY + 2, "WK", 0);
  drawBarCfg(f, barX, wkY + barDy, barW, layout.barH, layout.segW, wk);
  const wkPct = `${wk}%`;
  f.drawStr(ox + width - strW(wkPct) - 2, wkY + 2, wkPct, wk > 80);
}

function drawIconRow(f, rowY, iconFn, iconRed, label, value, valueRed) {
  iconFn(f, 6, rowY + 3, iconRed);
  f.drawStr(22, rowY + 4, label, 0);
  f.drawStr2x(102 - str2xW(value), rowY + 1, value, valueRed);
}

function renderDashboard() {
  const f = new Frame();
  const data = {
    githubPresent: true,
    github: { issues: 7, prs: 3, dependabot: 2, authError: false },
    claude: {
      fiveHour: { used: 84, limit: 200, resetInSeconds: 5400 },
      weekly: { used: 1240, limit: 5000, resetInSeconds: 205200 },
    },
    codex: { shortPct: 37, longPct: 27, reached: false, shortReset: 1823, longReset: 302400 },
    updatedAt: "21:35",
    stale: false,
    offline: false,
  };

  const claudeSes = Math.floor((data.claude.fiveHour.used * 100) / data.claude.fiveHour.limit);
  const claudeWk = Math.floor((data.claude.weekly.used * 100) / data.claude.weekly.limit);
  const codexSes = data.codex.shortPct;
  const codexWk = data.codex.longPct;
  const depsAlert = data.github.dependabot > 0;
  const authErr = data.github.authError;

  f.hline(1, 1, 294);
  f.hline(1, 126, 294);
  f.vline(1, 1, 126);
  f.vline(294, 1, 126);

  iconBoxLogo(f, 6, 4);
  f.drawStr(19, 5, "DEVDASH", 0);
  drawHeaderConnectionSlots(f, 1, 1);
  const next = "+5m";
  const xNext = 290 - strW(next);
  const xClock = xNext - 2 - strW(data.updatedAt);
  const xSync = xClock - 4 - 8;
  iconSync(f, xSync, 4);
  f.drawStr(xClock, 5, data.updatedAt, 0);
  f.drawStr(xNext, 5, next, 0);
  f.hline(2, 15, 292);

  const compactProvider = {
    titleRowH: 14,
    rowGap: 2,
    barRowH: 12,
    barH: 10,
    segW: 3,
    rowLabelW: 18,
    pctW: 28,
  };

  f.vline(106, 16, 110);
  drawIconRow(f, 20, iconIssue, 0, "ISSUES", String(data.github.issues), 0);
  drawIconRow(f, 42, iconPr, 0, "PRs", String(data.github.prs), 0);
  drawIconRow(f, 64, iconShield, depsAlert, "DEPS", depsAlert ? `${data.github.dependabot}!` : String(data.github.dependabot), depsAlert);
  drawIconRow(f, 86, iconKey, authErr, "AUTH", authErr ? "ERR" : "OK", authErr);
  drawProvider(f, 112, 20, 182, compactProvider, "CLAUDE", claudeSes, claudeWk, data.claude.fiveHour.resetInSeconds, data.claude.weekly.resetInSeconds);
  drawProvider(f, 112, 66, 182, compactProvider, "CODEX", codexSes, codexWk, data.codex.shortReset, data.codex.longReset);
  return f;
}

function drawBootHeader(f) {
  iconBoxLogo(f, 6, 4);
  f.drawStrAdv(19, 4, "DEVDASH", 1, FONT_BOOT_W);
  const status = "JOINING WIFI";
  f.drawStrAdv(291 - strWAdv(status, FONT_BOOT_W), 4, status, 1, FONT_BOOT_W);
  f.hline(2, 14, 293);
}

function drawBootSourceList(f, lx, ly) {
  f.drawStrAdv(lx, ly, "github", 1, FONT_BOOT_W);
  lx += strWAdv("github", FONT_BOOT_W) + FONT_BOOT_W;
  f.fillRect(lx + 1, ly + 3, 2, 2, 1, 0);
  lx += 2 * FONT_BOOT_W;
  f.drawStrAdv(lx, ly, "claude", 1, FONT_BOOT_W);
  lx += strWAdv("claude", FONT_BOOT_W) + FONT_BOOT_W;
  f.fillRect(lx + 1, ly + 3, 2, 2, 1, 0);
  lx += 2 * FONT_BOOT_W;
  f.drawStrAdv(lx, ly, "codex", 1, FONT_BOOT_W);
}

function drawBootFooter(f) {
  const fw = "fw v0.1.0";
  f.hline(2, 114, 293);
  f.drawStrAdv(6, 117, fw, 1, FONT_BOOT_W);

  const setup = "hold BOOT 5s -> setup";
  f.drawStrAdv(291 - strWAdv(setup, FONT_BOOT_W), 117, setup, 1, FONT_BOOT_W);
}

function drawBootNetworks(f, networks) {
  let row = 0;
  for (const ssid of networks) {
    if (row >= 5) break;
    const index = `${row + 1}.`;
    const y = 35 + row * 9;
    f.drawStrAdv(122, y, index, 1, FONT_BOOT_W);
    f.drawStrClippedAdv(137, y, ssid, 1, FONT_BOOT_W, 290 - 137 + 1);
    row++;
  }
}

function renderBoot() {
  const f = new Frame();
  f.hline(1, 1, 294);
  f.hline(1, 126, 294);
  f.vline(1, 1, 126);
  f.vline(294, 1, 126);

  drawBootHeader(f);
  f.fillRect(6, 20, 108, 90, 1, 0);
  f.drawStr4xBw(14, 30, "DEV", 0);
  f.drawStr4xBw(14, 63, "DASH", 0);
  f.drawStrAdv(14, 98, "sn 4C-12-A9", 0, FONT_BOOT_W);

  f.drawStrAdv(122, 22, "SCANNING SAVED NETWORKS", 1, FONT_BOOT_W);
  drawBootNetworks(f, ["HomeLab", "Office"]);
  f.hline(122, 82, 169);
  f.drawStrAdv(122, 88, "THEN FETCH", 1, FONT_BOOT_W);
  drawBootSourceList(f, 122, 101);

  drawBootFooter(f);
  return f;
}

function escapeXml(value) {
  return value.replace(/[&<>"']/g, (ch) => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    '"': "&quot;",
    "'": "&apos;",
  })[ch]);
}

const outDir = join(root, "docs/assets");
mkdirSync(outDir, { recursive: true });
writeFileSync(join(outDir, "readme-boot-screen.svg"), renderBoot().toSvg("DevDash boot screen"));
writeFileSync(join(outDir, "readme-dashboard-screen.svg"), renderDashboard().toSvg("DevDash dashboard screen"));
console.log("Rendered docs/assets/readme-boot-screen.svg");
console.log("Rendered docs/assets/readme-dashboard-screen.svg");
