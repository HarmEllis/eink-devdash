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
  if (glyphs.length !== 95) {
    throw new Error(`expected 95 glyphs, found ${glyphs.length}`);
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
    const safe = code < 32 || code > 126 ? "?".charCodeAt(0) : code;
    const glyph = font5x7[safe - 32];
    for (let col = 0; col < 5; col++) {
      for (let row = 0; row < 7; row++) {
        if (glyph[col] & (1 << row)) this.lpix(x + col, y + row, 1, useRed);
      }
    }
  }

  drawCharBw(x, y, ch, black) {
    const code = ch.charCodeAt(0);
    const safe = code < 32 || code > 126 ? "?".charCodeAt(0) : code;
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

  drawCharInv(x, y, ch) {
    const code = ch.charCodeAt(0);
    const safe = code < 32 || code > 126 ? "?".charCodeAt(0) : code;
    const glyph = font5x7[safe - 32];
    for (let col = 0; col < 5; col++) {
      for (let row = 0; row < 7; row++) {
        if (glyph[col] & (1 << row)) {
          this.lpix(x + col, y + row, 0, 0);
          this.lpix(x + col, y + row, 0, 1);
        }
      }
    }
  }

  drawStrInvAdv(x, y, text, advance) {
    for (const ch of text) {
      this.drawCharInv(x, y, ch);
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
    const safe = code < 32 || code > 126 ? "?".charCodeAt(0) : code;
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
    const safe = code < 32 || code > 126 ? "?".charCodeAt(0) : code;
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

  drawChar4xInv(x, y, ch) {
    const code = ch.charCodeAt(0);
    const safe = code < 32 || code > 126 ? "?".charCodeAt(0) : code;
    const glyph = font5x7[safe - 32];
    for (let col = 0; col < 5; col++) {
      for (let row = 0; row < 7; row++) {
        if (glyph[col] & (1 << row)) {
          this.fillRect(x + col * 4, y + row * 4, 4, 4, 0, 0);
          this.fillRect(x + col * 4, y + row * 4, 4, 4, 0, 1);
        }
      }
    }
  }

  drawStr4xInv(x, y, text) {
    for (const ch of text) {
      this.drawChar4xInv(x, y, ch);
      x += 23;
    }
  }

  drawChar3xInv(x, y, ch) {
    const code = ch.charCodeAt(0);
    const safe = code < 32 || code > 126 ? "?".charCodeAt(0) : code;
    const glyph = font5x7[safe - 32];
    for (let col = 0; col < 5; col++) {
      for (let row = 0; row < 7; row++) {
        if (glyph[col] & (1 << row)) {
          this.fillRect(x + col * 3, y + row * 3, 3, 3, 0, 0);
          this.fillRect(x + col * 3, y + row * 3, 3, 3, 0, 1);
        }
      }
    }
  }

  drawStr3xInv(x, y, text) {
    for (const ch of text) {
      this.drawChar3xInv(x, y, ch);
      x += 17;
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

// Mirrors firmware draw_currency_symbol: € needs its own 5x7 column-major
// bitmap (font5x7 stays ASCII-only); $ and unknown codes reuse the font glyph.
const glyphEuro = [0x3e, 0x55, 0x55, 0x41, 0x00];

function drawCurrencySymbol(f, x, y, code, useRed) {
  if (code === "EUR") {
    for (let col = 0; col < 5; col++) {
      for (let row = 0; row < 7; row++) {
        if (glyphEuro[col] & (1 << row)) f.lpix(x + col, y + row, 1, useRed);
      }
    }
    return;
  }
  f.drawChar(x, y, "$", useRed);
}

// Mirrors firmware format_spend_amount: up to 2 decimals, trailing zeros (and a
// bare trailing dot) trimmed: 0.91 -> "0.91", 1.00 -> "1", 1200 -> "1200". The
// decimal separator follows the currency (EUR -> comma, else dot).
function formatSpendAmount(amount, currency) {
  let a = amount < 0 ? 0 : amount > 99999 ? 99999 : amount;
  let s = a.toFixed(2);
  if (s.includes(".")) s = s.replace(/0+$/, "").replace(/\.$/, "");
  if (currency === "EUR" && s.includes(".")) s = s.replace(".", ",");
  return s;
}

function str2xW(text) {
  return text.length * FONT2_W;
}

function fitTextAscii(text, maxW) {
  const maxChars = Math.floor(Math.max(0, maxW) / FONT_BOOT_W);
  if (text.length <= maxChars) return text;
  if (maxChars <= 0) return "";
  if (maxChars <= 3) return text.slice(0, maxChars);
  return `${text.slice(0, maxChars - 3)}...`;
}

const PROVISION_SAMPLE = {
  ssid: "devdash-A1B2",
  password: "M9k3pX7vQ2nL",
  url: "192.168.4.1",
  qrRows: [
    "11111110010110111001001111111",
    "10000010000000010010101000001",
    "10111010111110101000001011101",
    "10111010100001011000001011101",
    "10111010110100101010101011101",
    "10000010110011011110101000001",
    "11111110101010101010101111111",
    "00000000111010001010100000000",
    "10111110000001111111001111100",
    "10110101001101000001001010110",
    "01100110000011110110011101000",
    "11000101000000010010011010011",
    "11111111001110101111000111100",
    "11100000101111011101111110110",
    "10100110101110110110100100100",
    "11001000100000010001010001000",
    "10001110001101011000100101011",
    "11001001110111101001111011010",
    "10011111000111011010100110000",
    "10101101000010100010000010001",
    "10110011101001011110111111100",
    "00000000111101100100100010100",
    "11111110010011011111101010100",
    "10000010101100100011100011011",
    "10111010110110000000111111011",
    "10111010101101001110000011011",
    "10111010111110111001001111110",
    "10000010011000010000100011010",
    "11111110100100101111011011000",
  ],
};

function iconBoxLogo(f, ox, oy) {
  f.fillRect(ox, oy, 9, 9, 1, 0);
  f.fillRect(ox + 2, oy + 2, 5, 5, 0, 0);
  f.fillRect(ox + 3, oy + 3, 3, 3, 1, 0);
}

function iconDownArrow(f, ox, oy) {
  const pts = [
    3, 0, 3, 1, 3, 2, 3, 3, 3, 4, 3, 5,
    1, 4, 2, 5, 3, 6, 4, 5, 5, 4,
  ];
  for (let i = 0; i < pts.length / 2; i++) {
    f.lpix(ox + pts[i * 2], oy + pts[i * 2 + 1], 1, 0);
  }
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

function iconInbox(f, ox, oy, useRed) {
  f.fillRect(ox + 1, oy + 1, 8, 1, 1, useRed);
  f.fillRect(ox + 0, oy + 2, 1, 6, 1, useRed);
  f.fillRect(ox + 9, oy + 2, 1, 6, 1, useRed);
  f.fillRect(ox + 1, oy + 7, 8, 1, 1, useRed);
  f.fillRect(ox + 2, oy + 5, 2, 1, 1, useRed);
  f.fillRect(ox + 6, oy + 5, 2, 1, 1, useRed);
  f.fillRect(ox + 4, oy + 6, 2, 1, 1, useRed);
}

function iconGithubMark(f, ox, oy, useRed) {
  f.fillRect(ox + 2, oy + 1, 6, 1, 1, useRed);
  f.fillRect(ox + 1, oy + 2, 8, 4, 1, useRed);
  f.fillRect(ox + 2, oy + 6, 6, 2, 1, useRed);
  f.fillRect(ox + 3, oy + 8, 1, 2, 1, useRed);
  f.fillRect(ox + 6, oy + 8, 1, 2, 1, useRed);
  f.fillRect(ox + 3, oy + 4, 1, 1, 0, 0);
  f.fillRect(ox + 6, oy + 4, 1, 1, 0, 0);
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

function iconCrossSync(f, ox, oy) {
  for (let i = 0; i < 8; i++) f.lpix(ox + i, oy + i, 1, 1);
  const d2 = [0, 7, 1, 6, 2, 5, 3, 4, 5, 3, 6, 2, 7, 1];
  for (let i = 0; i < 7; i++) f.lpix(ox + d2[i * 2], oy + d2[i * 2 + 1], 1, 1);
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

function iconGlobe(f, ox, oy) {
  f.fillRect(ox + 3, oy + 0, 4, 1, 1, 0);
  f.fillRect(ox + 2, oy + 1, 6, 1, 1, 0);
  f.fillRect(ox + 1, oy + 2, 2, 6, 1, 0);
  f.fillRect(ox + 7, oy + 2, 2, 6, 1, 0);
  f.fillRect(ox + 2, oy + 8, 6, 1, 1, 0);
  f.fillRect(ox + 3, oy + 9, 4, 1, 1, 0);
  f.fillRect(ox + 2, oy + 3, 6, 1, 1, 0);
  f.fillRect(ox + 2, oy + 6, 6, 1, 1, 0);
  f.fillRect(ox + 4, oy + 1, 2, 8, 1, 0);
  f.fillRect(ox + 4, oy + 2, 2, 1, 0, 0);
  f.fillRect(ox + 4, oy + 4, 2, 2, 0, 0);
  f.fillRect(ox + 4, oy + 7, 2, 1, 0, 0);
}

function iconWarningBig(f, ox, oy, useRed) {
  for (let y = 0; y < 24; y++) {
    const inset = Math.floor((23 - y) / 2);
    const w = 24 - inset * 2;
    if (w <= 0) continue;
    if (y === 23 || y > 18) {
      f.fillRect(ox + inset, oy + y, w, 1, 1, useRed);
    } else {
      f.fillRect(ox + inset, oy + y, 1, 1, 1, useRed);
      f.fillRect(ox + inset + w - 1, oy + y, 1, 1, 1, useRed);
    }
  }
  f.fillRect(ox + 11, oy + 8, 2, 8, 1, useRed);
  f.fillRect(ox + 11, oy + 19, 2, 2, 1, useRed);
}

function drawBarCfg(f, ox, oy, width, height, segW, pct, forceRed = false) {
  pct = Math.max(0, Math.min(100, pct));
  const stride = segW + 1;
  const cols = Math.floor((width + 1) / stride);
  const filled = Math.floor((pct * cols + 50) / 100);
  const thresh = Math.floor((80 * cols + 50) / 100);
  for (let i = 0; i < cols; i++) {
    const sx = ox + i * stride;
    if (i < filled) {
      const isRed = forceRed || (pct > 80 && i >= thresh);
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

function drawProvider(f, ox, oy, width, layout, title, ses, wk, sesResetS, wkResetS, extra = null) {
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

  if (extra !== null) {
    // Extra-usage row: [currency symbol] [bar = % of monthly cap] [amount].
    // The bar uses the real utilization percent; when absent (env override) it
    // falls back to an amount-capped fill. Mirrors firmware draw_provider:
    // prefer the API's preformatted valueText, else format the number locally.
    const amountText = extra.valueText ?? formatSpendAmount(extra.amount, extra.currency);
    const hasSpend = extra.amount > 0;
    let barPct;
    if (extra.percent != null) {
      barPct = extra.percent;
    } else {
      const amt = Math.round(extra.amount);
      barPct = amt < 0 ? 0 : amt > 100 ? 100 : amt;
    }
    const spendY = wkY + layout.barRowH + 1;
    // The amount can be wider than the fixed pct column, so shrink the bar to
    // end just before it rather than overlapping. Cap at barW so a short amount
    // leaves this bar at most as wide as the other two, never wider (mirrors
    // firmware).
    const amountX = ox + width - strW(amountText) - 2;
    const spendBarW = Math.max(0, Math.min(barW, amountX - 2 - barX));
    drawCurrencySymbol(f, ox, spendY + 1, extra.currency, 0);
    drawBarCfg(f, barX, spendY + barDy, spendBarW, layout.barH, layout.segW, barPct, hasSpend);
    f.drawStr(amountX, spendY + 1, amountText, hasSpend);
  }
}

function drawIconRow(f, rowY, iconFn, iconRed, label, value, valueRed) {
  iconFn(f, 6, rowY + 3, iconRed);
  f.drawStr(22, rowY + 4, label, 0);
  f.drawStr(102 - strW(value), rowY + 4, value, valueRed);
}

function formatCountValue(value) {
  return value > 999 ? "999+" : String(Math.max(0, value));
}

function drawGithubErrorColumn(f, label, useRed) {
  iconGithubMark(f, 6, 19, useRed);
  f.drawStr(20, 22, "GH", useRed);
  iconWarningBig(f, 51, 64, useRed);
  f.drawStr(54 - Math.floor(strW(label) / 2), 94, label, useRed);
}

function renderDashboard({ githubPresent = true, githubError = null } = {}) {
  const f = new Frame();
  const data = {
    githubPresent,
    github: { issues: 12, prs: 4, notifications: 1, dependabot: 0, authError: githubError === "auth" },
    claude: {
      fiveHour: { used: 18, limit: 200, resetInSeconds: 8200 },
      weekly: { used: 4100, limit: 10000, resetInSeconds: 304800 },
      extra: { amount: 0.91, percent: 5, limit: 17, currency: "EUR", valueText: "0,91" },
    },
    codex: {
      shortPct: 32,
      longPct: 38,
      reached: false,
      shortReset: 3600,
      longReset: 313200,
      extra: { amount: 3, percent: null, limit: null, currency: "USD", valueText: "3" },
    },
    updatedAt: "14:38",
    refreshMin: 5,
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
  const next = `+${data.refreshMin ?? 5}m`;
  const xNext = 290 - strW(next);
  const xClock = xNext - 2 - strW(data.updatedAt);
  const xSync = xClock - 4 - 8;
  iconSync(f, xSync, 4);
  f.drawStr(xClock, 5, data.updatedAt, 0);
  f.drawStr(xNext, 5, next, 0);
  f.hline(2, 15, 292);

  const compactProvider = {
    titleRowH: 10,
    rowGap: 0,
    barRowH: 11,
    barH: 9,
    segW: 3,
    rowLabelW: 18,
    pctW: 28,
  };

  if (data.githubPresent) {
    f.vline(106, 16, 110);
    if (githubError) {
      drawGithubErrorColumn(f, githubError === "auth" ? "AUTH FAIL" : "OFFLINE", 1);
    } else {
      iconGithubMark(f, 6, 19, 0);
      f.drawStr(20, 22, "GH", 0);
      drawIconRow(f, 42, iconIssue, 0, "ISS", formatCountValue(data.github.issues), 0);
      drawIconRow(f, 64, iconPr, 0, "PR", formatCountValue(data.github.prs), 0);
      let depY = 86;
      if (data.github.notifications !== null) {
        drawIconRow(f, 86, iconInbox, 0, "INBOX", formatCountValue(data.github.notifications), 0);
        depY = 108;
      }
      drawIconRow(f, depY, iconShield, depsAlert, "DEP", depsAlert ? `${data.github.dependabot}!` : formatCountValue(data.github.dependabot), depsAlert);
    }
    drawProvider(f, 112, 22, 182, compactProvider, "CLAUDE", claudeSes, claudeWk, data.claude.fiveHour.resetInSeconds, data.claude.weekly.resetInSeconds, data.claude.extra);
    drawProvider(f, 112, 76, 182, compactProvider, "CODEX", codexSes, codexWk, data.codex.shortReset, data.codex.longReset, data.codex.extra);
  } else {
    drawProvider(f, 4, 22, 288, compactProvider, "CLAUDE", claudeSes, claudeWk, data.claude.fiveHour.resetInSeconds, data.claude.weekly.resetInSeconds, data.claude.extra);
    drawProvider(f, 4, 76, 288, compactProvider, "CODEX", codexSes, codexWk, data.codex.shortReset, data.codex.longReset, data.codex.extra);
  }
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

function drawOtaHeader(f) {
  iconBoxLogo(f, 6, 4);
  f.drawStrAdv(19, 4, "DEVDASH", 1, FONT_BOOT_W);
  const label = "OTA UPDATE";
  const labelW = strWAdv(label, FONT_BOOT_W);
  const labelX = 290 - labelW;
  iconDownArrow(f, labelX - 11, 4);
  f.drawStrAdv(labelX, 4, label, 1, FONT_BOOT_W);
  f.hline(2, 14, 293);
}

function drawOtaIdentity(f, fromVersion, toVersion) {
  const pair = fitTextAscii(`fw ${fromVersion} > ${toVersion}`, 100);
  f.fillRect(6, 20, 108, 90, 1, 0);
  f.drawStr3xInv(9, 24, "OTA");
  f.drawStr3xInv(9, 54, "UPDATE");
  f.drawStrInvAdv(9, 98, pair, FONT_BOOT_W);
}

function drawOtaPlanRow(f, row, name, meta) {
  const y = 41 + row * 10;
  const idx = `${row + 1}.`;
  f.drawStrAdv(130 - strWAdv(idx, FONT_BOOT_W), y, idx, 1, FONT_BOOT_W);

  const metaW = strW(meta);
  f.drawStr(290 - metaW, y, meta, 0);

  const nameLeft = 134;
  const nameW = 290 - metaW - 4 - nameLeft;
  f.drawStrAdv(nameLeft, y, fitTextAscii(name, nameW), 1, FONT_BOOT_W);
}

function drawOtaFooterEta(f) {
  const eta = "~40s";
  const reboot = "reboot once";
  const etaW = strWAdv(eta, FONT_BOOT_W);
  const rebootW = strWAdv(reboot, FONT_BOOT_W);
  let lx = 290 - (etaW + 3 * FONT_BOOT_W + rebootW);

  f.drawStrAdv(lx, 117, eta, 1, FONT_BOOT_W);
  lx += etaW + FONT_BOOT_W;
  f.fillRect(lx + 1, 120, 2, 2, 1, 0);
  lx += 2 * FONT_BOOT_W;
  f.drawStrAdv(lx, 117, reboot, 1, FONT_BOOT_W);
}

function renderOta() {
  const f = new Frame();
  f.hline(1, 1, 294);
  f.hline(1, 126, 294);
  f.vline(1, 1, 126);
  f.vline(294, 1, 126);

  drawOtaHeader(f);
  drawOtaIdentity(f, "0.4.1", "0.5.0");

  f.drawStrAdv(122, 22, "INSTALLING UPDATE", 1, FONT_BOOT_W);
  f.hline(122, 35, 169);
  drawOtaPlanRow(f, 0, "download", "GitHub");
  drawOtaPlanRow(f, 1, "verify app", "image hdr");
  drawOtaPlanRow(f, 2, "flash app1", "partition B");
  drawOtaPlanRow(f, 3, "reboot", "boot B");

  f.hline(2, 114, 293);
  f.fillRect(6, 119, 3, 3, 1, 1);
  f.drawStr(13, 117, "DO NOT UNPLUG", 1);
  drawOtaFooterEta(f);
  return f;
}

function drawS1Chrome(f) {
  f.hline(1, 1, 294);
  f.hline(1, 126, 294);
  f.vline(1, 1, 126);
  f.vline(294, 1, 126);

  iconBoxLogo(f, 6, 4);
  f.drawStr(19, 5, "DEVDASH", 0);
  const setup = "SETUP";
  f.drawStr(290 - strW(setup), 5, setup, 0);

  f.hline(2, 15, 292);
  f.hline(2, 113, 292);

  const cap = "Scan with phone camera";
  f.drawStr((296 - strW(cap)) / 2, 117, cap, 0);
}

function drawS1InfoColumn(f, ssid, password, url) {
  iconWifi(f, 113, 22);
  f.drawStr(125, 26, "SSID", 0);
  f.drawStr(155, 26, ssid, 0);

  iconKey(f, 113, 44, 0);
  f.drawStr(125, 48, "PASS", 0);
  f.drawStr(155, 48, password, 0);

  iconGlobe(f, 113, 66);
  f.drawStr(125, 70, "URL", 0);
  f.drawStr(155, 70, url, 0);

  f.drawStr(115, 92, "join & popup opens", 0);
  f.drawStr(115, 101, "- or visit URL.", 0);
}

function drawQrRows(f, rows, originX, originY, paintSize) {
  const modules = rows.length;
  let modulePx = 3;
  while (modules * modulePx > paintSize - 2) {
    modulePx--;
    if (modulePx < 1) return;
  }
  const modulesPx = modules * modulePx;
  const offsetX = originX + Math.floor((paintSize - modulesPx) / 2);
  const offsetY = originY + Math.floor((paintSize - modulesPx) / 2);
  for (let y = 0; y < modules; y++) {
    for (let x = 0; x < rows[y].length; x++) {
      if (rows[y][x] === "1") {
        f.fillRect(offsetX + x * modulePx, offsetY + y * modulePx, modulePx, modulePx, 1, 0);
      }
    }
  }
}

function renderProvision() {
  const f = new Frame();
  drawS1Chrome(f);
  drawS1InfoColumn(f, PROVISION_SAMPLE.ssid, PROVISION_SAMPLE.password, PROVISION_SAMPLE.url);
  drawQrRows(f, PROVISION_SAMPLE.qrRows, 12, 20, 89);
  return f;
}

function drawOfflineChrome(f, label) {
  f.hline(1, 1, 294);
  f.hline(1, 126, 294);
  f.vline(1, 1, 126);
  f.vline(294, 1, 126);

  iconBoxLogo(f, 6, 4);
  f.drawStrAdv(19, 4, "DEVDASH", 1, FONT_BOOT_W);
  const labelX = 290 - strW(label);
  iconCrossSync(f, labelX - 13, 4);
  f.drawStr(labelX, 4, label, 1);
  f.hline(2, 14, 293);
  f.hline(2, 114, 293);
}

function drawOfflineIdentity(f, line2, footnote) {
  f.fillRect(6, 20, 108, 90, 1, 1);
  f.drawStr4xInv(14, 30, "NO");
  f.drawStr4xInv(14, 60, line2);
  f.drawStrInvAdv(14, 98, fitTextAscii(footnote, 92), FONT_BOOT_W);
}

function drawOfflineHeading(f, left, right) {
  let x = 122;
  f.drawStrAdv(x, 22, left, 1, FONT_BOOT_W);
  x += strWAdv(left, FONT_BOOT_W) + 6;
  f.fillRect(x + 1, 25, 2, 2, 1, 0);
  x += 10;
  f.drawStrAdv(x, 22, right, 1, FONT_BOOT_W);
  f.hline(122, 35, 169);
}

function drawFailureRows(f, rows) {
  rows.slice(0, 5).forEach((row, i) => {
    const y = 41 + i * 10;
    const idx = `${i + 1}.`;
    f.drawStrAdv(130 - strWAdv(idx, FONT_BOOT_W), y, idx, 1, FONT_BOOT_W);
    const reasonW = strW(row.reason);
    f.drawStr(290 - reasonW, y, row.reason, 1);
    const nameLeft = 134;
    const nameW = 290 - reasonW - 4 - nameLeft;
    f.drawStrAdv(nameLeft, y, fitTextAscii(row.name, nameW), 1, FONT_BOOT_W);
  });
}

function drawOfflineFooter(f, retry) {
  f.drawStr(6, 117, `retry ${retry}`, 1);
  const setup = "hold BOOT 5s -> setup";
  f.drawStrAdv(290 - strWAdv(setup, FONT_BOOT_W), 117, setup, 1, FONT_BOOT_W);
}

const OFFLINE_WIFI_SAMPLE = {
  retry: "30s",
  rows: [
    { name: "pixelhaus-2g", reason: "no-ap" },
    { name: "studio-mesh", reason: "auth-err" },
    { name: "iphone-hotspot", reason: "no-ap" },
    { name: "office-guest", reason: "timeout" },
    { name: "travel-router", reason: "no-ap" },
  ],
};

const OFFLINE_API_SAMPLE = {
  retry: "30s",
  ssid: "pixelhaus-2g",
  rows: [
    { name: "api.github.com", reason: "timeout" },
    { name: "api.anthropic.com", reason: "502" },
    { name: "chatgpt.com/backend", reason: "dns" },
    { name: "api.npmjs.org", reason: "refused" },
    { name: "hooks.slack.com", reason: "401" },
  ],
};

function renderOffline(kind) {
  const f = new Frame();
  if (kind === "wifi") {
    drawOfflineChrome(f, "NO WIFI");
    drawOfflineIdentity(f, "WIFI", `0/${OFFLINE_WIFI_SAMPLE.rows.length} joined`);
    drawOfflineHeading(f, "NETWORKS", "ALL FAILED");
    drawFailureRows(f, OFFLINE_WIFI_SAMPLE.rows);
    drawOfflineFooter(f, OFFLINE_WIFI_SAMPLE.retry);
  } else {
    drawOfflineChrome(f, "NO API");
    drawOfflineIdentity(f, "API", `on ${OFFLINE_API_SAMPLE.ssid}`);
    drawOfflineHeading(f, "UPSTREAMS", "ALL DOWN");
    drawFailureRows(f, OFFLINE_API_SAMPLE.rows);
    drawOfflineFooter(f, OFFLINE_API_SAMPLE.retry);
  }
  return f;
}

// ── Setup-mode reset flow (mirrors display.c draw_reset_* helpers) ──────────

function drawResetChrome(f, title) {
  f.hline(1, 1, 294);
  f.hline(1, 126, 294);
  f.vline(1, 1, 126);
  f.vline(294, 1, 126);

  iconBoxLogo(f, 6, 4);
  f.drawStr(19, 5, "DEVDASH", 0);
  f.drawStr(290 - strW(title), 5, title, 0);

  f.hline(2, 15, 292);
}

function renderResetConfirm() {
  const f = new Frame();
  drawResetChrome(f, "SETUP RESET");

  f.drawStr2x(6, 24, "TAP = CONFIG RESET", 0);
  f.drawStr(6, 42, "clears wifi + api + sleep; panel kept", 0);

  f.drawStr2x(6, 64, "HOLD 3s = FULL ERASE", 0);
  f.drawStr(6, 82, "wipes entire nvs; back to first run", 0);

  f.hline(2, 100, 292);
  f.drawStr(6, 106, "WAIT 15s = CANCEL", 0);
  return f;
}

function renderResetBanner(title, banner, l1, l2, footer) {
  const f = new Frame();
  drawResetChrome(f, title);
  f.drawStr4xBw((296 - banner.length * 23) / 2, 34, banner, 1);
  f.drawStr((296 - strW(l1)) / 2, 74, l1, 0);
  f.drawStr((296 - strW(l2)) / 2, 86, l2, 0);
  f.hline(2, 104, 292);
  f.drawStr((296 - strW(footer)) / 2, 115, footer, 0);
  return f;
}

function renderResetFail() {
  const f = new Frame();
  drawResetChrome(f, "SETUP RESET");
  f.drawStr2x((296 - str2xW("NVS WRITE FAILED")) / 2, 42, "NVS WRITE FAILED", 0);
  f.drawStr((296 - strW("config not cleared - nvs full?")) / 2, 66, "config not cleared - nvs full?", 0);
  f.hline(2, 104, 292);
  f.drawStr((296 - strW("BOOT = RETRY / WAIT 10s = BACK")) / 2, 115, "BOOT = RETRY / WAIT 10s = BACK", 0);
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
const screens = [
  ["readme-boot-screen.svg", renderBoot(), "DevDash boot screen"],
  ["readme-dashboard-screen.svg", renderDashboard(), "DevDash dashboard screen"],
  ["readme-ota-screen.svg", renderOta(), "DevDash OTA update screen"],
  ["readme-provision-screen.svg", renderProvision(), "DevDash provisioning screen"],
  ["readme-no-wifi-screen.svg", renderOffline("wifi"), "DevDash no WiFi screen"],
  ["readme-api-error-screen.svg", renderOffline("api"), "DevDash API error screen"],
  ["reset-confirm-screen.svg", renderResetConfirm(), "DevDash setup reset confirm"],
  ["reset-result-config-screen.svg", renderResetBanner("CONFIG RESET", "DONE", "wifi + api + sleep cleared", "panel + refresh settings kept", "returning to setup..."), "DevDash config reset done"],
  ["reset-result-erase-screen.svg", renderResetBanner("FULL ERASE", "WIPING", "entire nvs erased on reboot", "device restarts as first run", "rebooting..."), "DevDash full erase"],
  ["reset-result-fail-screen.svg", renderResetFail(), "DevDash setup reset NVS failure"],
];

for (const [filename, frame, title] of screens) {
  writeFileSync(join(outDir, filename), frame.toSvg(title));
  console.log(`Rendered docs/assets/${filename}`);
}
