#pragma once

// Shared DIP metrics for the custom-drawn chrome.
//
// One place for every size that layout, drawing and hit-testing must agree on.
// All values are DIP (1/96 inch) and are multiplied by the window DPI scale in
// layout.cpp; never hard-code a physical pixel size for these.

namespace nv::ui::metrics {

inline constexpr float kTitleHeight = 32.0f;
inline constexpr float kBottomHeight = 44.0f;
inline constexpr float kStripHeight = 76.0f;
// v3.0 (規劃/26 §2.10): the reveal area is now the rail slot + the bottom bar.
// This band survives only for the `--hover-hotzone` hook and for the
// "pressing in the hidden rail's band must not start a pan" guard.
inline constexpr float kStripHotZone = 8.0f;
// Hover tooltip above a thumbnail slot (規劃/22/08 §3.3).
inline constexpr float kStripTooltipMaxWidth = 320.0f;
inline constexpr float kStripTooltipMinWidth = 48.0f;
inline constexpr float kStripTooltipHeight = 26.0f;
inline constexpr float kStripTooltipPadding = 8.0f;
inline constexpr float kStripTooltipGap = 6.0f;
inline constexpr float kStripTooltipMargin = 4.0f;
inline constexpr float kStripTooltipFontSize = 12.0f;
inline constexpr float kStripTooltipRadius = 4.0f;

inline constexpr float kRadiusWindow = 10.0f;   // physical pixels (DWM)
inline constexpr float kRadiusControl = 6.0f;

// Fullscreen reveal (v3.0, 規劃/26 §2.7): two square buttons floating in the
// top-right corner, with the same margin as the title bar's own padding.
inline constexpr float kFullscreenButton = 40.0f;
inline constexpr float kFullscreenMargin = 12.0f;
inline constexpr float kFullscreenGap = 8.0f;
// The pointer may be this far outside the buttons and still reveal/keep them.
inline constexpr float kFullscreenHotPad = 24.0f;

// Bird's eye view (v3.0, 規劃/26 §2.4). The panel is an overlay above the rail;
// its size is bounded so an opened bird's eye can never become the picture.
inline constexpr float kMinimapMaxWidth = 220.0f;
inline constexpr float kMinimapMaxHeight = 180.0f;
// Below this the panel is not placed at all (tiny window / extreme aspect):
// a one-pixel preview would be a fake entry, not a control.
inline constexpr float kMinimapMinWidth = 80.0f;
inline constexpr float kMinimapMinHeight = 16.0f;
inline constexpr float kMinimapMargin = 12.0f;
// Gap to the rail slot it floats above; the fullscreen variant uses the margin.
inline constexpr float kMinimapGap = 8.0f;
inline constexpr float kMinimapPadding = 3.0f;
inline constexpr float kMinimapRadius = 6.0f;
inline constexpr float kMinimapFrameStroke = 1.5f;

// Title bar.
inline constexpr float kTitlePaddingLeft = 12.0f;
inline constexpr float kTitlePaddingRight = 12.0f;
inline constexpr float kTitleButtonWidth = 46.0f;
inline constexpr float kTitleButtonWidthCompact = 40.0f;
inline constexpr float kTitleIconSize = 20.0f;
inline constexpr float kTitleIconGap = 8.0f;
inline constexpr float kTitleSegmentGap = 12.0f;
inline constexpr float kTitleFontSize = 12.0f;
inline constexpr float kTitleButtonIconSize = 16.0f;
inline constexpr float kTitleButtonIconStroke = 1.5f;
inline constexpr float kCompactWidth = 640.0f;

// Bottom bar.
inline constexpr float kBottomPadding = 12.0f;
inline constexpr float kBottomFontSize = 12.0f;
inline constexpr float kBottomIconSize = 16.0f;
inline constexpr float kPartInfo = 28.0f;
inline constexpr float kPartPercent = 56.0f;
inline constexpr float kPartIndex = 72.0f;
inline constexpr float kPartFit = 64.0f;
inline constexpr float kPartActual = 56.0f;
inline constexpr float kPartRotate = 40.0f;
inline constexpr float kPartMore = 88.0f;

// Overlay stack (see 規劃/22/20).
inline constexpr float kStackGap = 12.0f;
inline constexpr float kPillHeight = 36.0f;
inline constexpr float kPillGap = 12.0f;
// Frame control of animated images (規劃/22/12 §3.1): one row on the bottom
// stack, 36 DIP tall, with three 28 DIP buttons and a centred frame counter.
inline constexpr float kAnimPillWidth = 248.0f;
inline constexpr float kAnimPillPadding = 10.0f;
inline constexpr float kAnimPillButton = 28.0f;
inline constexpr float kAnimPillButtonGap = 6.0f;
inline constexpr float kAnimPillIconSize = 16.0f;
inline constexpr float kAnimPillFontSize = 12.0f;
inline constexpr float kAnimPillRadius = 18.0f;
inline constexpr float kAnimPillRestOpacity = 0.35f;
// Two idle seconds before the pill dims; the fade itself takes 200 ms
// (規劃/22/12 §3.1).
inline constexpr float kAnimPillIdleMs = 2000.0f;
inline constexpr float kAnimPillFadeMs = 200.0f;
inline constexpr float kAnimPillTickMs = 40.0f;
inline constexpr float kToastHeight = 36.0f;
inline constexpr float kToastGap = 16.0f;
inline constexpr float kInfoWidth = 360.0f;
inline constexpr float kInfoGap = 16.0f;
// The card may grow to 70% of the image area before the body scrolls
// (規劃/22/06 §4.1).
inline constexpr float kInfoMaxHeightRatio = 0.7f;

// Info panel (規劃/22/02 §3.3, EXIF groups in 22/06). The height is derived
// from the visible line count instead of a fixed box so the panel never draws
// dead space. One line is either a group header or a label/value row; both are
// the same height so the layout never needs a second measurement pass.
inline constexpr float kInfoHeaderHeight = 30.0f;
inline constexpr float kInfoRowHeight = 24.0f;
inline constexpr float kInfoPadding = 12.0f;
inline constexpr float kInfoFooterHeight = 44.0f;
inline constexpr float kInfoCloseSize = 22.0f;
inline constexpr float kInfoLabelWidth = 96.0f;
inline constexpr float kInfoTitleFontSize = 13.0f;
inline constexpr float kInfoRowFontSize = 11.0f;
inline constexpr float kInfoGroupFontSize = 10.0f;
inline constexpr float kInfoButtonHeight = 28.0f;
inline constexpr float kInfoButtonPadding = 14.0f;
inline constexpr float kInfoButtonMinWidth = 84.0f;
inline constexpr float kInfoScrollbarWidth = 3.0f;
inline constexpr float kInfoBodyGap = 8.0f;

// Popup menus anchored to a bottom-bar part (規劃/22/02 §3.3).
inline constexpr float kMenuWidth = 240.0f;
inline constexpr float kMenuWidthCompact = 168.0f;
inline constexpr float kMenuRowHeight = 32.0f;
inline constexpr float kMenuPadding = 8.0f;
inline constexpr float kMenuGap = 4.0f;
inline constexpr float kMenuMargin = 8.0f;
inline constexpr float kMenuRowInset = 4.0f;
inline constexpr float kMenuFontSize = 12.0f;
inline constexpr float kMenuToggleWidth = 36.0f;
inline constexpr float kMenuToggleHeight = 20.0f;
inline constexpr float kMenuToggleKnob = 16.0f;
inline constexpr float kMenuToggleInset = 2.0f;
inline constexpr float kMenuCheckSize = 14.0f;
inline constexpr float kMenuSeparatorHeight = 1.0f;
// v3.0 (規劃/26 §2.9): a separator owns a compact row instead of a full command
// row, so the right-click menu's three groups do not read as empty 32 DIP rows.
inline constexpr float kMenuSeparatorRowHeight = 10.0f;

// Image-area overlays (規劃/22/09 §5.1). 56 DIP circles, 16 DIP from the
// content edges, vertically centred.
inline constexpr float kArrowSize = 56.0f;
inline constexpr float kArrowMargin = 16.0f;
inline constexpr float kArrowIconSize = 20.0f;
inline constexpr float kArrowIconStroke = 1.5f;
// Resting opacity; hovering the circle brings it to 1.0. Below 0.2 the circle
// is not hit-testable (坑 7); the resting value is above that on purpose.
inline constexpr float kArrowRestOpacity = 0.35f;

// "Preview" badge while a slow decode still runs (規劃/22/13 §3.5): top-right
// corner of the image area, 22 DIP tall, fully rounded, never hit-testable.
inline constexpr float kPreviewBadgeMargin = 12.0f;
inline constexpr float kPreviewBadgeHeight = 22.0f;
inline constexpr float kPreviewBadgePadding = 10.0f;
inline constexpr float kPreviewBadgeMinWidth = 48.0f;
inline constexpr float kPreviewBadgeFontSize = 11.0f;
inline constexpr float kPreviewBadgeMaxWidth = 240.0f;
// Fade-out once the full image arrives (160 ms, same beat as the auto rail).
inline constexpr float kPreviewBadgeFadeMs = 160.0f;
inline constexpr float kPreviewBadgeTickMs = 30.0f;

// Startup empty state (規劃/22/03 §3.1). The card is centred in the content
// rectangle (title/bottom rail already subtracted) and its height follows the
// format-line count and the optional note.
inline constexpr float kEmptyCardMaxWidth = 520.0f;
inline constexpr float kEmptyCardMargin = 24.0f;
inline constexpr float kEmptyCardPadding = 28.0f;
inline constexpr float kEmptyCardRadius = 8.0f;
inline constexpr float kEmptyIconSize = 64.0f;
inline constexpr float kEmptyIconGap = 20.0f;
inline constexpr float kEmptyTitleHeight = 28.0f;
inline constexpr float kEmptyTitleGap = 12.0f;
inline constexpr float kEmptyFormatLineHeight = 18.0f;
inline constexpr float kEmptyFormatsMaxLines = 2;
inline constexpr float kEmptyMoreGap = 4.0f;
inline constexpr float kEmptyMoreHeight = 16.0f;
inline constexpr float kEmptyNoteGap = 6.0f;
inline constexpr float kEmptySectionGap = 22.0f;
inline constexpr float kEmptyButtonWidth = 132.0f;
inline constexpr float kEmptyButtonHeight = 36.0f;
inline constexpr float kEmptyFooterGap = 14.0f;
inline constexpr float kEmptyFooterHeight = 24.0f;
inline constexpr float kEmptyFooterWidth = 264.0f;
inline constexpr float kEmptyFooterButtonWidth = 120.0f;
inline constexpr float kEmptyFooterButtonGap = 24.0f;
inline constexpr float kEmptyHintHeight = 28.0f;
inline constexpr float kEmptyHintTopGap = 24.0f;
inline constexpr float kEmptyHintWidth = 180.0f;

// Empty-state typography (DIP; DrawText multiplies by DPI once).
inline constexpr float kEmptyTitleFontSize = 20.0f;
inline constexpr float kEmptyFormatsFontSize = 12.0f;
inline constexpr float kEmptyMoreFontSize = 11.0f;
inline constexpr float kEmptyButtonFontSize = 13.0f;
inline constexpr float kEmptyFooterFontSize = 12.0f;
inline constexpr float kEmptyNoteFontSize = 12.0f;

// "All formats" modal typography.
inline constexpr float kFormatsTitleFontSize = 14.0f;
inline constexpr float kFormatsRowFontSize = 11.0f;
inline constexpr float kFormatsNoteFontSize = 10.0f;

// "All formats" modal card (規劃/22/03 §3.3). One row per family.
inline constexpr float kFormatsPanelWidth = 420.0f;
inline constexpr float kFormatsPanelMaxHeight = 420.0f;
inline constexpr float kFormatsPanelMargin = 16.0f;
inline constexpr float kFormatsHeaderHeight = 30.0f;
inline constexpr float kFormatsRowHeight = 18.0f;
inline constexpr float kFormatsPadding = 16.0f;
inline constexpr float kFormatsFooterHeight = 28.0f;
inline constexpr float kFormatsNameWidth = 96.0f;
inline constexpr float kFormatsNoteWidth = 104.0f;
inline constexpr float kFormatsCloseWidth = 72.0f;
inline constexpr float kFormatsCloseHeight = 26.0f;

// Window.
inline constexpr float kMinWindowWidth = 640.0f;
inline constexpr float kMinWindowHeight = 480.0f;
inline constexpr float kResizeBorder = 6.0f;

// --- Error / confirmation cards and loading state (規劃/22/11) -------------
inline constexpr float kErrorCardMaxWidth = 480.0f;
inline constexpr float kErrorCardMargin = 24.0f;
inline constexpr float kErrorCardPadding = 20.0f;
inline constexpr float kErrorCardRadius = 8.0f;
inline constexpr float kErrorTitleFontSize = 15.0f;
inline constexpr float kErrorBodyFontSize = 12.0f;
inline constexpr float kErrorDetailFontSize = 11.0f;
inline constexpr float kErrorIconSize = 20.0f;
inline constexpr float kErrorTitleHeight = 24.0f;
inline constexpr float kErrorBodyHeight = 20.0f;
inline constexpr float kErrorDetailHeight = 18.0f;
inline constexpr float kErrorRowGap = 6.0f;
inline constexpr float kErrorSectionGap = 16.0f;
inline constexpr float kErrorButtonHeight = 32.0f;
inline constexpr float kErrorButtonMinWidth = 84.0f;
inline constexpr float kErrorButtonMaxWidth = 150.0f;
inline constexpr float kErrorButtonGap = 8.0f;
inline constexpr float kErrorButtonPadding = 12.0f;

inline constexpr float kConfirmCardWidth = 440.0f;
inline constexpr float kConfirmCardMargin = 24.0f;
inline constexpr float kConfirmPadding = 20.0f;
inline constexpr float kConfirmTitleHeight = 24.0f;
inline constexpr float kConfirmBodyHeight = 40.0f;
inline constexpr float kConfirmButtonWidth = 112.0f;
inline constexpr float kConfirmButtonHeight = 32.0f;
inline constexpr float kConfirmButtonGap = 10.0f;
// Optional "remember my choice" row (規劃/22/10 §4.3).
inline constexpr float kConfirmRememberHeight = 20.0f;
inline constexpr float kConfirmRememberBox = 16.0f;
inline constexpr float kConfirmRememberGap = 8.0f;

// Rename overlay (V3.1-06 §06e): one-line input box plus an error line, same
// card metrics as the confirmation card.
inline constexpr float kRenameInputHeight = 34.0f;
inline constexpr float kRenameErrorHeight = 20.0f;

inline constexpr float kLoadingBarWidth = 120.0f;
inline constexpr float kLoadingBarHeight = 3.0f;
inline constexpr float kLoadingChipHeight = 68.0f;
inline constexpr float kLoadingLabelHeight = 18.0f;
inline constexpr float kLoadingLabelGap = 10.0f;
inline constexpr float kLoadingLabelWidth = 160.0f;

// --- Save options card (規劃/22/14 §5) -------------------------------------
inline constexpr float kSaveCardWidth = 360.0f;
inline constexpr float kSaveCardMargin = 24.0f;
inline constexpr float kSavePadding = 20.0f;
inline constexpr float kSaveCardRadius = 8.0f;
inline constexpr float kSaveTitleHeight = 24.0f;
inline constexpr float kSaveRowHeight = 36.0f;
inline constexpr float kSaveRowGap = 8.0f;
inline constexpr float kSaveEstimateHeight = 24.0f;
inline constexpr float kSaveSectionGap = 16.0f;
inline constexpr float kSaveButtonWidth = 88.0f;
inline constexpr float kSaveButtonHeight = 32.0f;
inline constexpr float kSaveButtonGap = 10.0f;
inline constexpr float kSaveLabelWidth = 76.0f;
inline constexpr float kSaveValueWidth = 44.0f;
inline constexpr float kSaveTrackHeight = 4.0f;
inline constexpr float kSaveKnob = 12.0f;
inline constexpr float kSaveTitleFontSize = 14.0f;
inline constexpr float kSaveFontSize = 12.0f;

// --- Settings window (規劃/22/07 §4) ---------------------------------------
inline constexpr float kSettingsWidth = 760.0f;
inline constexpr float kSettingsHeight = 560.0f;
inline constexpr float kSettingsMinWidth = 680.0f;
inline constexpr float kSettingsMinHeight = 480.0f;
inline constexpr float kSettingsTitleHeight = 32.0f;
inline constexpr float kSettingsTitlePadding = 16.0f;
inline constexpr float kSettingsTitleFontSize = 13.0f;
inline constexpr float kSettingsCloseSize = 22.0f;
inline constexpr float kSettingsNavWidth = 160.0f;
inline constexpr float kSettingsNavRowHeight = 40.0f;
inline constexpr float kSettingsNavPadding = 16.0f;
inline constexpr float kSettingsNavFontSize = 12.0f;
inline constexpr float kSettingsNavBarWidth = 3.0f;
inline constexpr float kSettingsPadding = 20.0f;
inline constexpr float kSettingsLabelWidth = 120.0f;
inline constexpr float kSettingsRowHeight = 36.0f;
inline constexpr float kSettingsGroupHeight = 28.0f;
inline constexpr float kSettingsNoteHeight = 32.0f;
inline constexpr float kSettingsFontSize = 12.0f;
inline constexpr float kSettingsGroupFontSize = 10.0f;
inline constexpr float kSettingsCheckSize = 18.0f;
inline constexpr float kSettingsRadioSize = 18.0f;
inline constexpr float kSettingsRadioDot = 8.0f;
inline constexpr float kSettingsOptionGap = 18.0f;
inline constexpr float kSettingsOptionInnerGap = 6.0f;
inline constexpr float kSettingsSliderWidth = 160.0f;
inline constexpr float kSettingsSliderValueWidth = 44.0f;
inline constexpr float kSettingsSliderTrack = 4.0f;
inline constexpr float kSettingsSliderKnob = 12.0f;
inline constexpr float kSettingsFooterHeight = 56.0f;
inline constexpr float kSettingsButtonHeight = 32.0f;
inline constexpr float kSettingsButtonMinWidth = 88.0f;
inline constexpr float kSettingsButtonPadding = 14.0f;
inline constexpr float kSettingsButtonGap = 10.0f;
inline constexpr float kSettingsScrollbarWidth = 3.0f;
inline constexpr float kSettingsContentGap = 0.0f;

// Association page.
inline constexpr float kAssociationHeaderHeight = 34.0f;
inline constexpr float kAssociationRowHeight = 30.0f;
inline constexpr float kAssociationColumns = 4.0f;
inline constexpr float kAssociationButtonHeight = 30.0f;
inline constexpr float kAssociationStatusHeight = 22.0f;
inline constexpr float kAssociationListGap = 12.0f;


}  // namespace nv::ui
