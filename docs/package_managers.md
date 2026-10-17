# Package managers and installed components

Installed CMake packages expose `sqlconduit_AVAILABLE_COMPONENTS` and `sqlconduit_SCHEMA_FILE`:

```cmake
find_package(sqlconduit REQUIRED COMPONENTS Core)
message(STATUS "Available components: ${sqlconduit_AVAILABLE_COMPONENTS}")
message(STATUS "Configuration Schema: ${sqlconduit_SCHEMA_FILE}")
target_link_libraries(app PRIVATE sqlconduit::core)
```

Only request components that the application links. Available drivers depend on how SQLConduit was
built. A driver package still needs its client SDK on the consumer machine; Core has no SDK
requirement. OpenTelemetry is separately requested and rediscovers `opentelemetry-cpp` only when used.

## vcpkg SDKs (Windows)

Use a vcpkg baseline/lock appropriate for your project, then install the SDKs you need:

```powershell
vcpkg install libmysql:x64-windows libpqxx:x64-windows
cmake -S . -B build-vcpkg -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DSQLCONDUIT_ENABLE_MYSQL=ON -DSQLCONDUIT_ENABLE_POSTGRES=ON `
  -DSQLCONDUIT_ENABLE_ODBC=ON -DSQLCONDUIT_BUILD_TESTS=ON
cmake --build build-vcpkg --config Release
cmake --install build-vcpkg --config Release --prefix install
```

This consumes SQLConduit from source using vcpkg-provided dependencies; it does not imply a
SQLConduit port is published in the vcpkg registry. Windows ODBC uses `odbc32` from the Windows SDK;
install a runtime driver separately. SQLConduit chooses imported libpqxx/libpq targets so Debug and
Release link the matching SDK artifacts. libpqxx 8 requires C++20 for the driver compilation while
the Core/application public headers remain C++17. Do not mix SDK architectures or runtime-library
modes. See the official [libmysql](https://vcpkg.io/en/package/libmysql) and
[libpqxx](https://vcpkg.io/en/package/libpqxx) port pages for supported versions/triplets.

## Conan 2 local Core package

The root `conanfile.py` creates a local Core-only package from this checkout. This path does not
bundle vendor SDKs or claim that the recipe has been published to ConanCenter:

```sh
conan profile detect
conan create . -s build_type=Release -s compiler.cppstd=17 --build=missing
```

In a consuming project's `conanfile.txt`:

```ini
[requires]
sqlconduit/1.1.0

[generators]
CMakeDeps
CMakeToolchain
```

```sh
conan install . --output-folder=build -s build_type=Release --build=missing
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Use the ordinary `find_package(sqlconduit COMPONENTS Core)` shown above. The recipe preserves the
project's installed config instead of replacing its component and Schema metadata with a generated
facade. Build database components from source with their SDK discovery settings when required.

## Discovery and troubleshooting

For missing SDK errors the configure output now gives OS-specific installation guidance, the
relevant `*_INCLUDE_DIR`/`*_LIBRARY` variables, and the Core-only alternative. Common remedies:

- macOS: add keg-only Homebrew `mysql-client`, `libpq`, `libpqxx`, and `unixodbc` prefixes to
  `CMAKE_PREFIX_PATH`.
- Linux: install development headers and libraries, not only runtime client command-line tools.
- Windows: use one architecture/triplet consistently and pass the vcpkg toolchain on the first
  configure of a fresh build directory.
- Oracle: install Instant Client Basic and SDK separately; pass `OCI_INCLUDE_DIR` and `OCI_LIBRARY`.
  Prebuilt SQLConduit archives do not redistribute OCI.

Install Debug and Release into the same prefix only with compatible toolchains. Debug libraries use
the `d` postfix. Independent installed-consumer tests must exercise each selected component and both
MSVC configurations before release.
