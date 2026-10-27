#pragma once

#include <windows.h>

#include <string>
#include <utility>

namespace nv {

struct Error {
  int code = 0;
  HRESULT hr = S_OK;
  std::string message;

  bool ok() const { return code == 0 && SUCCEEDED(hr); }

  static Error Ok() { return {}; }

  static Error Make(int code_value, std::string text, HRESULT result = S_OK) {
    Error error;
    error.code = code_value;
    error.hr = result;
    error.message = std::move(text);
    return error;
  }
};

template <typename T>
struct Result {
  T value{};
  Error error;

  bool ok() const { return error.ok(); }
};

}  // namespace nv
