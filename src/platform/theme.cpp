#include "platform/theme.h"

#include <cstdint>

namespace nv::platform {
namespace {

int g_forced = -1;

}  // namespace

bool IsHighContrastEnabled() {
  if (g_forced >= 0) return g_forced != 0;
  HIGHCONTRASTW high_contrast{};
  high_contrast.cbSize = sizeof(high_contrast);
  if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(high_contrast),
                             &high_contrast, 0)) {
    return false;
  }
  return (high_contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

void SetForcedHighContrast(int value) {
  g_forced = value < 0 ? -1 : (value != 0 ? 1 : 0);
}

int ForcedHighContrast() { return g_forced; }

uint32_t SystemColorRef(int system_color_index) {
  return static_cast<uint32_t>(GetSysColor(system_color_index));
}

}  // namespace nv::platform
