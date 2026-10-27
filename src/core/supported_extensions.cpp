#include "core/supported_extensions.h"

#include <algorithm>

namespace nv {
namespace {

bool IsIconExtension(const std::wstring& extension) {
  return extension == L".ico" || extension == L".cur";
}

}  // namespace

const std::vector<std::wstring>& SupportedExtensions() {
  static const std::vector<std::wstring> extensions = {
      L".jpg",   L".jpeg", L".jpe",   L".jfif", L".png",   L".apng",
      L".bmp",   L".dib",  L".gif",   L".webp", L".tif",   L".tiff",
      L".ico",   L".cur",  L".psd",   L".psb",  L".heic",  L".heif",
      L".hif",   L".avif", L".avifs", L".svg",  L".tga",   L".targa",
      L".pcx",   L".pbm",  L".pgm",   L".ppm",  L".pnm",   L".xbm",
      L".xpm",   L".wbmp", L".hdr",   L".exr",  L".pfm",   L".jp2",
      L".j2k",   L".jpc",  L".jxr",   L".wdp",  L".mng",   L".jng",
      L".cr2",   L".cr3",  L".nef",   L".arw",  L".raf",   L".orf",
      L".rw2",   L".pef",  L".srw",   L".dng",
  };
  return extensions;
}

bool IsSupportedImageExtension(const std::wstring& extension) {
  const std::vector<std::wstring>& extensions = SupportedExtensions();
  return std::ranges::find(extensions, extension) != extensions.end();
}

const std::vector<std::wstring>& ImageDocumentExtensions() {
  static const std::vector<std::wstring> extensions = [] {
    std::vector<std::wstring> result;
    for (const std::wstring& extension : SupportedExtensions()) {
      if (!IsIconExtension(extension)) result.push_back(extension);
    }
    return result;
  }();
  return extensions;
}

bool IsImageDocumentExtension(const std::wstring& extension) {
  const std::vector<std::wstring>& extensions = ImageDocumentExtensions();
  return std::ranges::find(extensions, extension) != extensions.end();
}

}  // namespace nv
