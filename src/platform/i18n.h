#pragma once

#include <string>
#include <string_view>

namespace nv::i18n {

// Every user-visible string in the GUI lives in the table in i18n.cpp. Add new
// entries before kCount and fill both languages; ui_tests checks that no entry
// is empty and that English strings contain no CJK characters.
enum class StringId {
  kMenuOpenFile,
  kMenuSaveAs,
  kMenuCopyImage,
  kMenuCopyPath,
  kMenuRename,
  kMenuDelete,
  kMenuWallpaper,
  kMenuReveal,
  kMenuLanguage,
  kLanguageChinese,
  kLanguageEnglish,

  kDialogOpenImage,
  kDialogSaveAs,
  kDialogRename,
  kDialogDelete,

  kErrorOpenImage,
  kErrorSaveImage,
  kErrorCopyImage,
  kErrorCopyPath,
  kErrorDelete,
  kErrorRename,
  kErrorWallpaper,
  kErrorReveal,

  kErrorInvalidFileName,
  kErrorUnsupportedSaveExtension,
  kErrorUnsupportedRenameExtension,
  kErrorFileExists,
  kErrorWallpaperDirectory,
  kDeleteConfirmation,
  kRenamePrompt,

  kFilterImages,
  kFilterAllFiles,
  kFilterPng,
  kFilterJpeg,
  kFilterBmp,
  kFilterTiff,

  kAnimationSuffixFormat,
  kAnimationPlaying,
  kAnimationPaused,
  kLoadingTitleFormat,

  kButtonOk,
  kButtonCancel,

  kShellVerbOpen,
  kShellVerbWallpaper,
  kShellVerbCopy,
  kAssociationFriendlyName,
  kShortcutDescription,

  kErrorLocalDiskOnly,
  kErrorWallpaperMissing,
  kErrorImageTooLarge,
  kErrorNotBgra8,
  kErrorInvalidStride,
  kErrorSaveArguments,
  kErrorPathEmpty,
  kWarningUserChoiceFormat,

  // v2.0 custom-drawn chrome.
  kUiFit,
  kUiMore,

  // v2.0 S1: bottom bar commands, more panel, info panel, toasts (規劃/22/02).
  kUiFitToWindow,
  kUiBirdseye,
  kUiThumbnails,
  kUiOn,
  kUiOff,
  kUiInfoTitle,
  kInfoFileName,
  kInfoDimensions,
  kInfoFileSize,
  kInfoFormat,
  kInfoIndex,
  kInfoZoom,
  kInfoViewRotationFormat,
  kToastPathCopied,
  // V3.1-06 (§06d): "Copy image" needs the same visible confirmation the path
  // copy has; it used to succeed silently.
  kToastImageCopied,
  kToastDimensionCopied,
  kToastFileSizeCopied,
  kToastToggleFormat,

  // v2.0 S1: startup empty state, folder opens and the all-formats card
  // (規劃/22/03).
  kEmptyTitle,
  kEmptyFormatsPrefix,
  kEmptyFormatsMore,
  kEmptyChooseImage,
  kEmptyOpenFolder,
  kEmptyAllFormats,
  kEmptyFolderTitle,
  kEmptyFolderHint,
  kEmptyUnsupported,
  kEmptyDragHint,
  kEmptyFormatsTitle,
  kEmptyFormatsClose,
  kEmptyFormatsHint,
  kEmptyFormatsNeedCodec,
  kEmptyFormatsNotBuilt,

  // v2.0 S1: error card, confirmation card, loading state (規劃/22/11).
  kErrorActionRetry,
  kErrorActionCopyPath,
  kErrorActionOpenWith,
  kErrorActionReveal,
  kErrorActionClose,
  kErrorFileMissing,
  kErrorFileMissingHint,
  kErrorAccessDenied,
  kErrorAccessDeniedHint,
  kErrorUnsupported,
  kErrorUnsupportedHint,
  kErrorCorrupt,
  kErrorCorruptHint,
  kErrorCodecMissing,
  kErrorCodecMissingHint,
  kErrorOutOfMemory,
  kErrorOutOfMemoryHint,
  kErrorSaveFailedHint,
  kErrorOperationFailed,
  kErrorOperationFailedHint,
  kErrorRenderFailed,
  kErrorRenderFailedHint,
  kLoadingText,
  kButtonDelete,
  kConfirmDeleteBody,
  kToastImageSaved,
  kToastWallpaperSet,
  kToastFileDeleted,

  // v2.0 S2: EXIF info panel (規劃/22/06).
  kInfoGroupFile,
  kInfoGroupImage,
  kInfoGroupCamera,
  kInfoGroupColor,
  kInfoName,
  kInfoPath,
  kInfoModified,
  kInfoDepth,
  kInfoFrames,
  kInfoCamera,
  kInfoLens,
  kInfoTaken,
  kInfoExposure,
  kInfoAperture,
  kInfoIso,
  kInfoFocal,
  kInfoBias,
  kInfoFlash,
  kInfoMetering,
  kInfoWhiteBalance,
  kInfoGps,
  kInfoIcc,
  kInfoCopyAll,
  kInfoClose,
  kInfoSecondsUnit,
  kInfoAlphaNone,
  kInfoAlphaYes,
  kInfoFlashFired,
  kInfoFlashNoFire,
  kInfoMeterAverage,
  kInfoMeterCenter,
  kInfoMeterSpot,
  kInfoMeterMultiSpot,
  kInfoMeterPattern,
  kInfoMeterPartial,
  kInfoWbAuto,
  kInfoWbManual,
  kInfoSrgb,
  kInfoUncalibrated,
  kInfoIccConvertedFormat,
  kInfoDepthFormat,
  kInfoValueFormat,
  kInfoNone,
  kInfoReading,
  kInfoCopied,
  kInfoCopiedAll,

  // v2.0 S2: settings window and model (規劃/22/07).
  kSettingsMenu,
  kSettingsTitle,
  kSettingsPageGeneral,
  kSettingsPageView,
  kSettingsPageAssociation,
  kSettingsGroupInterface,
  kSettingsGroupThumbnails,
  kSettingsGroupWindow,
  kSettingsGroupZoom,
  kSettingsGroupFullscreen,
  kSettingsLanguage,
  kSettingsTheme,
  kSettingsFollowSystem,
  kSettingsThemeDark,
  kSettingsThemeLight,
  kSettingsChecker,
  kSettingsCheckerOpacity,
  kSettingsStrip,
  kSettingsStripAuto,
  kSettingsStripPinned,
  kSettingsStripOff,
  kSettingsBirdseye,
  kSettingsRememberWindow,
  kSettingsSingleInstance,
  kSettingsTopmost,
  kSettingsWheel,
  kSettingsWheelZoom,
  kSettingsWheelPage,
  kSettingsZoomStep,
  kSettingsPixelMode,
  kSettingsKeepRotation,
  kSettingsFullscreenBackground,
  kSettingsFullscreenTheme,
  kSettingsFullscreenBlack,
  kSettingsFullscreenDim,
  kSettingsFolderWrap,
  kSettingsWrapLoop,
  kSettingsWrapNext,
  kSettingsWrapAsk,
  kSettingsReset,
  kSettingsResetConfirm,
  kSettingsResetHint,
  kSettingsOpenConfig,
  kSettingsClose,
  kSettingsAssocCountFormat,
  kSettingsAssocSelectAll,
  kSettingsAssocRegister,
  kSettingsAssocUnregister,
  kSettingsAssocDefault,
  kSettingsAssocDefaultConfirm,
  kSettingsAssocDefaultHint,
  kSettingsAssocRegisteredFormat,
  kSettingsAssocFailedFormat,
  kSettingsAssocNone,
  kSettingsAssocDefaultIsOurs,
  kSettingsAssocDefaultOther,
  kSettingsRestored,
  kSettingsSaveFailed,
  kSettingsZoomStepFormat,
  kSettingsRestartNote,

  // Folder wrap prompt ("ask" boundary policy).
  kConfirmWrapEndTitle,
  kConfirmWrapEndBody,
  kConfirmWrapStartTitle,
  kConfirmWrapStartBody,
  kConfirmWrapContinue,
  // Folder watching (規劃/22/10).
  kToastFileRemoved,
  kToastFileModified,
  kConfirmWrapNextFolderFormat,
  kConfirmWrapPrevFolderFormat,
  kConfirmWrapEnter,
  kConfirmWrapStay,
  kConfirmWrapRemember,

  // Animation frame pill (規劃/22/12 §3.1/§3.4).
  kAnimEnded,
  kAnimTruncated,
  kAnimFirstFrame,

  // Two-stage loading (規劃/22/13 §3.5): badge text and the toast that blocks
  // exports while only the preview exists.
  kPreviewBadge,
  kPreviewBadgeFailed,
  kPreviewBusy,

  // Save-as enhancement (規劃/22/14): the options card, the WebP/GIF filters
  // and the save-in-progress / current-frame toasts.
  kFilterWebp,
  kFilterGif,
  kSaveTitleFormat,
  kSaveQualityLabel,
  kSaveEstimateLabel,
  kSaveEstimateFormat,
  kSaveEstimateComputing,
  kSaveEstimateUnavailable,
  kSaveButton,
  kSaveSavingText,
  kToastImageSavedFrame,

  // Drag-out (規劃/22/19): one-time hint when Ctrl+click does not move.
  kToastDragHint,

  // Product name shown in the title bar / about box (零式 / Zore).
  kAppTitle,
  kSaveCardOriginal,
  kSaveCardQuality,
  kSaveCardEstimate,

  kCount,
};

enum class Language {
  kChinese,
  kEnglish,
};

// Follows the Windows display language: Chinese UI language -> Chinese,
// everything else -> English.
Language SystemLanguage();

void SetLanguage(Language language);
Language CurrentLanguage();

// Accepts "zh", "zh-CN", "zh-Hans", "chinese" and "en", "en-US", "english"
// (case-insensitive).
bool ParseLanguage(std::wstring_view text, Language* language);

// "zh" / "en" (for settings.ini and command-line round trips).
const wchar_t* LanguageCode(Language language);

// Native display name: 中文 / English.
const wchar_t* LanguageNativeName(Language language);

const wchar_t* Text(StringId id);
const wchar_t* Text(Language language, StringId id);

}  // namespace nv::i18n
