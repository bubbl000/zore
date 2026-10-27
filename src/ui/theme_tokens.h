#pragma once

#include <cstdint>

namespace nv::ui {

// Colour tokens for the custom-drawn chrome. 0xAARRGGBB, so a token can be
// handed to D2D after a single shift and the same value can be asserted by
// unit tests.
//
// Nothing outside this file may hard-code a UI colour: high contrast must be
// able to replace every token with a system colour (規劃/22/01 §3.3).
struct ColorTokens {
  uint32_t bg_window = 0xFF121417;
  uint32_t bg_panel = 0xFF1A1D21;
  uint32_t bg_panel_inactive = 0xFF16181B;
  uint32_t bg_canvas = 0xFF0E1013;
  uint32_t bg_hover = 0xFF232830;
  uint32_t bg_pressed = 0xFF2C333D;

  uint32_t border_subtle = 0xFF2A2F36;
  uint32_t border_strong = 0xFF3A424C;

  uint32_t text_primary = 0xFFE8EAED;
  uint32_t text_secondary = 0xFFA0A6AD;
  uint32_t text_muted = 0xFF6B7280;
  uint32_t text_inactive = 0xFF8A9199;

  // Decision 2026-10-23: the accent is this green, not the system blue.
  uint32_t accent = 0xFF90C208;
  uint32_t accent_hover = 0xFFA6D80E;
  uint32_t accent_pressed = 0xFF78A206;
  // Text/icon colour drawn on top of `accent` (primary buttons).
  uint32_t on_accent = 0xFFFFFFFF;

  uint32_t danger = 0xFFF05252;
  uint32_t danger_hover = 0xFFFF6B6B;

  // Dimming behind an in-window modal card (all-formats list).
  uint32_t scrim = 0x99000000;

  uint32_t checker_a = 0xFF1B1F24;
  uint32_t checker_b = 0xFF262C33;
};

struct TokenSet {
  ColorTokens color;
  bool dark = true;
  bool high_contrast = false;
};

// Dark / light palettes are constants; high contrast reads the live system
// colours (COLOR_WINDOW / COLOR_WINDOWTEXT / COLOR_HIGHLIGHT / COLOR_BTNFACE),
// so it must be re-evaluated after WM_SETTINGCHANGE / WM_SYSCOLORCHANGE.
TokenSet TokensFor(bool dark_theme, bool high_contrast);

// Multiplies the alpha channel (used for the 160 ms fades of the auto strip and
// toasts). Values are clamped to 0..1.
uint32_t WithAlpha(uint32_t argb, float alpha);

// 0xAARRGGBB -> 0x00BBGGRR (GDI COLORREF) and back.
uint32_t ArgbToColorRef(uint32_t argb);
uint32_t ColorRefToArgb(uint32_t colorref);

}  // namespace nv::ui
