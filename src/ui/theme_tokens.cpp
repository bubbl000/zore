#include "ui/theme_tokens.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace nv::ui {
namespace {

uint32_t Opaque(uint32_t rgb) { return 0xFF000000u | (rgb & 0x00FFFFFFu); }

ColorTokens DarkPalette() {
  ColorTokens c;
  c.bg_window = Opaque(0x121417);
  c.bg_panel = Opaque(0x1A1D21);
  c.bg_panel_inactive = Opaque(0x16181B);
  c.bg_canvas = Opaque(0x0E1013);
  c.bg_hover = Opaque(0x232830);
  c.bg_pressed = Opaque(0x2C333D);
  c.border_subtle = Opaque(0x2A2F36);
  c.border_strong = Opaque(0x3A424C);
  c.text_primary = Opaque(0xE8EAED);
  c.text_secondary = Opaque(0xA0A6AD);
  c.text_muted = Opaque(0x6B7280);
  c.text_inactive = Opaque(0x8A9199);
  c.accent = Opaque(0x90C208);
  c.accent_hover = Opaque(0xA6D80E);
  c.accent_pressed = Opaque(0x78A206);
  c.danger = Opaque(0xF05252);
  c.danger_hover = Opaque(0xFF6B6B);
  c.on_accent = Opaque(0xFFFFFF);
  c.checker_a = Opaque(0x1B1F24);
  c.checker_b = Opaque(0x262C33);
  return c;
}

ColorTokens LightPalette() {
  ColorTokens c;
  c.bg_window = Opaque(0xF2F4F6);
  c.bg_panel = Opaque(0xFFFFFF);
  c.bg_panel_inactive = Opaque(0xF6F7F9);
  c.bg_canvas = Opaque(0xFAFBFC);
  c.bg_hover = Opaque(0xE8EBEF);
  c.bg_pressed = Opaque(0xD8DDE5);
  c.border_subtle = Opaque(0xE1E5EA);
  c.border_strong = Opaque(0xC4CBD4);
  c.text_primary = Opaque(0x1B1F24);
  c.text_secondary = Opaque(0x5A6470);
  c.text_muted = Opaque(0x8B949E);
  c.text_inactive = Opaque(0x6E7883);
  // The dark-theme green is too light on white; this is the same hue family
  // pushed down until the text/icon contrast passes 4.5:1.
  c.accent = Opaque(0x6F9A05);
  c.accent_hover = Opaque(0x7FAE06);
  c.accent_pressed = Opaque(0x5D8204);
  c.danger = Opaque(0xC81E1E);
  c.danger_hover = Opaque(0xE03030);
  c.on_accent = Opaque(0xFFFFFF);
  c.checker_a = Opaque(0xFFFFFF);
  c.checker_b = Opaque(0xE9ECEF);
  return c;
}

uint32_t SystemArgb(int index) {
  return ColorRefToArgb(static_cast<uint32_t>(GetSysColor(index)));
}

ColorTokens HighContrastPalette() {
  ColorTokens c;
  const uint32_t window = SystemArgb(COLOR_WINDOW);
  const uint32_t window_text = SystemArgb(COLOR_WINDOWTEXT);
  const uint32_t highlight = SystemArgb(COLOR_HIGHLIGHT);
  const uint32_t highlight_text = SystemArgb(COLOR_HIGHLIGHTTEXT);
  const uint32_t button_face = SystemArgb(COLOR_BTNFACE);
  // Disabled/muted text must stay readable on the window background, so it is
  // the system grey-text colour, never COLOR_HIGHLIGHTTEXT (which is meant for
  // text drawn on the highlight background and would vanish here).
  const uint32_t gray_text = SystemArgb(COLOR_GRAYTEXT);

  c.bg_window = window;
  c.bg_panel = button_face;
  c.bg_panel_inactive = button_face;
  c.bg_canvas = window;
  // Hover/pressed must stay visible on an arbitrary system palette, so they are
  // the highlight colour rather than a grey.
  c.bg_hover = highlight;
  c.bg_pressed = highlight;
  c.border_subtle = window_text;
  c.border_strong = window_text;
  c.text_primary = window_text;
  c.text_secondary = window_text;
  c.text_muted = gray_text;
  c.text_inactive = window_text;
  c.accent = highlight;
  c.accent_hover = highlight;
  c.accent_pressed = highlight;
  c.danger = window_text;
  c.danger_hover = highlight;
  c.on_accent = highlight_text;
  c.checker_a = window;
  c.checker_b = window;
  return c;
}

}  // namespace

TokenSet TokensFor(bool dark_theme, bool high_contrast) {
  TokenSet tokens;
  if (high_contrast) {
    tokens.color = HighContrastPalette();
    tokens.dark = false;
    tokens.high_contrast = true;
    return tokens;
  }
  tokens.color = dark_theme ? DarkPalette() : LightPalette();
  tokens.dark = dark_theme;
  tokens.high_contrast = false;
  return tokens;
}

uint32_t WithAlpha(uint32_t argb, float alpha) {
  const float clamped = std::clamp(alpha, 0.0f, 1.0f);
  const uint32_t a = static_cast<uint32_t>(std::lround(clamped * 255.0f));
  return (argb & 0x00FFFFFFu) | (a << 24);
}

uint32_t ArgbToColorRef(uint32_t argb) {
  const uint32_t r = (argb >> 16) & 0xFFu;
  const uint32_t g = (argb >> 8) & 0xFFu;
  const uint32_t b = argb & 0xFFu;
  return (b << 16) | (g << 8) | r;
}

uint32_t ColorRefToArgb(uint32_t colorref) {
  const uint32_t r = colorref & 0xFFu;
  const uint32_t g = (colorref >> 8) & 0xFFu;
  const uint32_t b = (colorref >> 16) & 0xFFu;
  return 0xFF000000u | (r << 16) | (g << 8) | b;
}

}  // namespace nv::ui
