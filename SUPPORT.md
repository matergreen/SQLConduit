# Version support

SQLConduit uses semantic versioning from 1.0 onward. Support means that the project evaluates
applicable reports and may publish a patch; it does not promise an SLA.

| Version line | Status | Support level |
| --- | --- | --- |
| `1.0.x` | Supported | Security, correctness, build, and documented compatibility fixes |
| `1.0.0-rc.x` | Superseded | Upgrade to the final 1.0.x release |
| `0.7.x` | Transition | Security fixes for 90 days after the 1.0.0 release |
| `0.8.x`, `0.9.x` | Development snapshots | Upgrade to 1.0; these were not production support lines |
| `0.6.x` and older | Unsupported | Upgrade before reporting a version-specific issue |

The 1.0 release freezes the public source and configuration contracts. Future `1.x` minor lines
preserve those contracts. When a
new minor line is released, the preceding minor receives security and critical correctness fixes for
at least 90 days. Patch releases do not shorten an existing support window. See
[the API stability contract](docs/api_stability.md) for the precise boundary.

## Supported build platforms

The release gate builds and tests the following host/toolchain combinations:

| Platform | Toolchain | Release archive |
| --- | --- | --- |
| Ubuntu 24.04 x86-64 | GCC and Clang | GCC archive |
| Ubuntu 24.04 arm64 | GCC | arm64 archive |
| macOS 14 arm64 | Apple Clang | arm64 archive |
| Windows x64 | Visual Studio 2022 / MSVC | x64 archive |

Other C++17 platforms and newer compatible toolchains are best effort. SQLConduit ships static
libraries and promises source compatibility across `1.x`; it does not promise C++ binary ABI
compatibility across compilers, standard-library implementations, runtime-library modes, or
toolchain versions. Build the library and application with compatible settings.

## Database validation

The 1.0 release is validated against these representative combinations:

| SQLConduit component | Validated server/client path | Validation |
| --- | --- | --- |
| MySQL | MySQL 8.4 with libmysqlclient-compatible headers | Automated live integration |
| PostgreSQL | PostgreSQL 16+ with libpq/libpqxx | Automated live integration |
| ODBC / SQL Server | SQL Server 2022 through unixODBC and FreeTDS | Automated live Linux integration; the Microsoft ODBC Driver path has Windows compile/test validation only in 1.0.0 |
| Oracle | Oracle Database Free with OCI 23 | Manual release integration |

These are validation targets, not artificial minimum server versions. Other versions are supported
when both the database vendor and client SDK support them, but reports must include a reproduction.
Microsoft ODBC Driver live execution is not part of the 1.0.0 validated support matrix; its ODBC
path is compile-tested on Windows, while live SQL Server behavior is validated with FreeTDS.
Oracle is not included in prebuilt archives because redistributing the OCI SDK is outside this
project's release workflow; build the Oracle component from source against an installed Instant
Client SDK.

Prebuilt Linux and macOS archives contain Core, MySQL, PostgreSQL, and ODBC components. The Windows
archive contains Core and ODBC. Components are separate static libraries so consumers only need the
client SDKs for drivers they actually link. Exact behavioral boundaries are listed in
[Known limitations](docs/known_limitations.md); implementation, validation, and planned-work status
is tracked in [Feature coverage and evolution](docs/feature_coverage.md).

Reproductions should state the SQLConduit version, OS, compiler, database server, client SDK/ODBC
driver, enabled component, configuration, and whether an asynchronous result reported `Native` or
`CompatibilityFallback`.
