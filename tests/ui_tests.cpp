// M3-3: localization table, language parsing, DPI helpers and high-contrast
// state. These run without a window or D3D device.

#include "tests/ui_tests.h"

#include "platform/dpi.h"
#include "platform/i18n.h"
#include "platform/theme.h"
#include "render/thumbnail_strip.h"

#include <cmath>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

namespace nv::tests {
namespace {

int g_failures = 0;

void Check(bool ok, const char* name) {
  if (!ok) {
    std::printf("FAIL %s\n", name);
    ++g_failures;
  }
}

bool HasCjk(const wchar_t* text) {
  for (const wchar_t* p = text; *p != L'\0'; ++p) {
    if (*p >= 0x4E00 && *p <= 0x9FFF) return true;
    if (*p >= 0x3000 && *p <= 0x303F) return true;  // CJK punctuation
  }
  return false;
}

}  // namespace

int RunUiTests() {
  using nv::i18n::Language;
  using nv::i18n::StringId;

  const int count = static_cast<int>(StringId::kCount);
  Check(count > 50, "i18n table has a plausible size");
  int cjk_strings = 0;
  for (int i = 0; i < count; ++i) {
    const auto id = static_cast<StringId>(i);
    const wchar_t* zh = nv::i18n::Text(Language::kChinese, id);
    const wchar_t* en = nv::i18n::Text(Language::kEnglish, id);
    Check(zh != nullptr && zh[0] != L'\0', "every Chinese string is non-empty");
    Check(en != nullptr && en[0] != L'\0', "every English string is non-empty");
    // The language picker intentionally shows "中文" in both languages.
    if (id != StringId::kLanguageChinese) {
      Check(!HasCjk(en), "English strings contain no CJK characters");
    }
    if (HasCjk(zh)) ++cjk_strings;
  }
  Check(cjk_strings > 30, "most Chinese strings are actually translated");

  Check(nv::i18n::Text(StringId::kMenuOpenFile)[0] == L'打',
        "Chinese menu text is selected when the language is Chinese");
  nv::i18n::SetLanguage(Language::kEnglish);
  Check(std::wstring(nv::i18n::Text(StringId::kMenuOpenFile)) == L"Open file…",
        "English menu text follows SetLanguage");
  nv::i18n::SetLanguage(Language::kChinese);
  Check(std::wstring(nv::i18n::Text(StringId::kButtonOk)) == L"确定",
        "language can be switched back to Chinese");

  Language language = Language::kEnglish;
  Check(nv::i18n::ParseLanguage(L"zh-CN", &language) &&
            language == Language::kChinese,
        "ParseLanguage accepts zh-CN");
  Check(nv::i18n::ParseLanguage(L"EN-us", &language) &&
            language == Language::kEnglish,
        "ParseLanguage accepts EN-us");
  Check(nv::i18n::ParseLanguage(L"chinese", &language) &&
            language == Language::kChinese,
        "ParseLanguage accepts chinese");
  Check(!nv::i18n::ParseLanguage(L"fr", &language),
        "ParseLanguage rejects unknown languages");
  Check(std::wstring(nv::i18n::LanguageCode(Language::kChinese)) == L"zh" &&
            std::wstring(nv::i18n::LanguageCode(Language::kEnglish)) == L"en",
        "language codes round-trip through settings.ini");
  Check(std::wstring(nv::i18n::LanguageNativeName(Language::kEnglish)) ==
            L"English",
        "native language name is English");

  // Format strings must accept the arguments the UI passes.
  wchar_t buffer[256]{};
  std::swprintf(
      buffer, std::size(buffer),
      nv::i18n::Text(Language::kEnglish, StringId::kAnimationSuffixFormat),
      L"GIF", static_cast<size_t>(4),
      nv::i18n::Text(Language::kEnglish, StringId::kAnimationPlaying));
  Check(std::wstring(buffer) == L"  [GIF 4 frames Playing]",
        "animation suffix format is localized");
  std::swprintf(
      buffer, std::size(buffer),
      nv::i18n::Text(Language::kChinese, StringId::kWarningUserChoiceFormat),
      static_cast<size_t>(3));
  Check(std::wcsstr(buffer, L"3") != nullptr,
        "UserChoice warning formats the extension count");

  // DPI conversion and the forced-scale hook.
  Check(nv::platform::DpiPixels(76.0f, 1.0f) == 76, "76 DIP at 100% is 76 px");
  Check(nv::platform::DpiPixels(76.0f, 1.25f) == 95,
        "76 DIP at 125% is 95 px");
  Check(nv::platform::DpiPixels(76.0f, 1.5f) == 114,
        "76 DIP at 150% is 114 px");
  Check(nv::platform::DpiPixels(76.0f, 2.0f) == 152,
        "76 DIP at 200% is 152 px");
  nv::platform::SetForcedDpiScale(1.5f);
  Check(std::fabs(nv::platform::DpiScaleForWindow(nullptr) - 1.5f) < 0.001f,
        "forced DPI scale overrides the window DPI");
  nv::platform::SetForcedDpiScale(0.0f);

  // The strip lays out in physical pixels at every DPI.
  for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
    nv::render::ThumbnailStrip strip;
    std::vector<std::filesystem::path> items;
    for (int i = 0; i < 40; ++i) {
      items.emplace_back(L"item" + std::to_wstring(i) + L".png");
    }
    strip.SetItems(items);
    strip.SetDpiScale(scale);
    strip.SetViewWidth(1280.0f);
    Check(std::fabs(strip.height() - 76.0f * scale) < 0.01f,
          "strip height scales with DPI");
    size_t hit = nv::render::ThumbnailStrip::kNone;
    const float strip_height = strip.height();
    Check(strip.HitTest(20.0f * scale, 800.0f - strip_height * 0.5f, 1280.0f,
                        800.0f, &hit) &&
              hit == 0,
          "first item is hittable at every DPI");
    std::vector<size_t> visible;
    strip.VisibleIndices(2, &visible);
    Check(!visible.empty(), "visible range is non-empty at every DPI");
    Check(visible.back() < items.size(), "visible range stays in bounds");
  }

  // High contrast is a forced testable state around the system setting.
  nv::platform::SetForcedHighContrast(1);
  Check(nv::platform::IsHighContrastEnabled(), "forced high contrast is on");
  nv::platform::SetForcedHighContrast(0);
  Check(!nv::platform::IsHighContrastEnabled(), "forced high contrast is off");
  nv::platform::SetForcedHighContrast(-1);
  Check(nv::platform::ForcedHighContrast() == -1,
        "high contrast can return to following the system");

  if (g_failures == 0) {
    std::printf("ok   UI/i18n/DPI checks\n");
  }
  return g_failures;
}

}  // namespace nv::tests
