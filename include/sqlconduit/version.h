#ifndef SQLCONDUIT_VERSION_H
#define SQLCONDUIT_VERSION_H

#define SQLCONDUIT_VERSION_MAJOR 1
#define SQLCONDUIT_VERSION_MINOR 0
#define SQLCONDUIT_VERSION_PATCH 0
#define SQLCONDUIT_VERSION_PRERELEASE ""
#define SQLCONDUIT_VERSION_STRING "1.0.0"

namespace sqlconduit {
    inline constexpr int versionMajor = SQLCONDUIT_VERSION_MAJOR;
    inline constexpr int versionMinor = SQLCONDUIT_VERSION_MINOR;
    inline constexpr int versionPatch = SQLCONDUIT_VERSION_PATCH;
    inline constexpr const char *versionPrerelease = SQLCONDUIT_VERSION_PRERELEASE;
    inline constexpr const char *versionString = SQLCONDUIT_VERSION_STRING;
}

#endif
