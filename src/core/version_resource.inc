// Shared VERSIONINFO body. Each <target>.rc defines:
//   NV_RC_FILE_DESCRIPTION, NV_RC_INTERNAL_NAME,
//   NV_RC_ORIGINAL_FILENAME, NV_RC_FILETYPE
// then includes this file. version_numbers.h supplies the version/product
// strings shared with src/core/version.h.
#include <winver.h>

#include "version_numbers.h"

#ifndef NV_RC_FILE_DESCRIPTION
#error "NV_RC_FILE_DESCRIPTION must be defined before including version_resource.inc"
#endif
#ifndef NV_RC_INTERNAL_NAME
#error "NV_RC_INTERNAL_NAME must be defined before including version_resource.inc"
#endif
#ifndef NV_RC_ORIGINAL_FILENAME
#error "NV_RC_ORIGINAL_FILENAME must be defined before including version_resource.inc"
#endif
#ifndef NV_RC_FILETYPE
#define NV_RC_FILETYPE VFT_APP
#endif

VS_VERSION_INFO VERSIONINFO
 FILEVERSION NV_VERSION_MAJOR, NV_VERSION_MINOR, NV_VERSION_PATCH, NV_VERSION_BUILD
 PRODUCTVERSION NV_VERSION_MAJOR, NV_VERSION_MINOR, NV_VERSION_PATCH, NV_VERSION_BUILD
 FILEFLAGSMASK VS_FFI_FILEFLAGSMASK
 FILEFLAGS 0x0L
 FILEOS VOS_NT_WINDOWS32
 FILETYPE NV_RC_FILETYPE
 FILESUBTYPE VFT2_UNKNOWN
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName", NV_VERSION_COMPANY
            VALUE "FileDescription", NV_RC_FILE_DESCRIPTION
            VALUE "FileVersion", NV_VERSION_STRING
            VALUE "InternalName", NV_RC_INTERNAL_NAME
            VALUE "LegalCopyright", NV_VERSION_COPYRIGHT
            VALUE "OriginalFilename", NV_RC_ORIGINAL_FILENAME
            VALUE "ProductName", NV_VERSION_PRODUCT
            VALUE "ProductVersion", NV_VERSION_STRING
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
