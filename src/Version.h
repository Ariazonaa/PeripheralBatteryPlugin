#pragma once

// Single place for the version number; also read by the resource compiler (VERSIONINFO).
#define PBP_VERSION_MAJOR 1
#define PBP_VERSION_MINOR 6
#define PBP_VERSION_PATCH 1
#define PBP_VERSION_STRING "1.6.1"

#ifndef RC_INVOKED
constexpr wchar_t kPluginVersion[] = L"" PBP_VERSION_STRING;
#endif
