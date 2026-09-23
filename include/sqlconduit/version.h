#ifndef SQLCONDUIT_VERSION_H
#define SQLCONDUIT_VERSION_H

#define SQLCONDUIT_VERSION_MAJOR 0
#define SQLCONDUIT_VERSION_MINOR 7
#define SQLCONDUIT_VERSION_PATCH 0
#define SQLCONDUIT_VERSION_STRING "0.7.0"

namespace sqlconduit {
    inline constexpr int versionMajor = SQLCONDUIT_VERSION_MAJOR;
    inline constexpr int versionMinor = SQLCONDUIT_VERSION_MINOR;
    inline constexpr int versionPatch = SQLCONDUIT_VERSION_PATCH;
    inline constexpr const char *versionString = SQLCONDUIT_VERSION_STRING;
}

#endif
