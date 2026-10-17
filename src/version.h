#pragma once

#define PRACCY_VERSION_MAJOR 2
#define PRACCY_VERSION_MINOR 6
#define PRACCY_VERSION_PATCH 0
#define PRACCY_VERSION_BUILD 6
#define PRACCY_VERSION_RAW   "2.6-beta6"
#define PRACCY_VERSION_STR   "v" PRACCY_VERSION_RAW

#ifdef __cplusplus
namespace praccy {

// Current application version - Single Source of Truth for all Praccy C++ components
inline constexpr const char* kVersionString = PRACCY_VERSION_STR;
inline constexpr const char* kVersionRaw    = PRACCY_VERSION_RAW;

inline constexpr int kVersionMajor = PRACCY_VERSION_MAJOR;
inline constexpr int kVersionMinor = PRACCY_VERSION_MINOR;
inline constexpr int kVersionPatch = PRACCY_VERSION_PATCH;
inline constexpr int kVersionBuild = PRACCY_VERSION_BUILD;

} // namespace praccy
#endif
