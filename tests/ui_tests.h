#pragma once

namespace nv::tests {

// i18n table/lookup checks plus DPI and high-contrast helpers. Returns the
// number of failed checks so the caller can fold them into its own total.
int RunUiTests();

}  // namespace nv::tests
