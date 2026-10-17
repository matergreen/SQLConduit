from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy
from conan.tools.build import check_min_cppstd
import os


class SQLConduitConan(ConanFile):
    """Local Core package. Vendor driver SDKs are intentionally separate."""
    name = "sqlconduit"
    version = "1.1.0-dev"
    license = "Apache-2.0"
    package_type = "static-library"
    settings = "os", "arch", "compiler", "build_type"
    exports_sources = "CMakeLists.txt", "cmake/*", "src/*", "include/*", "third_party/*", "config/*", "LICENSE", "tools/*"

    def layout(self):
        cmake_layout(self)

    def validate(self):
        check_min_cppstd(self, 17)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["SQLCONDUIT_BUILD_TESTS"] = False
        tc.variables["SQLCONDUIT_BUILD_BENCHMARKS"] = False
        tc.variables["SQLCONDUIT_BUILD_TOOLS"] = False
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        CMake(self).install()
        copy(self, "LICENSE", self.source_folder, os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        # Use SQLConduit's original relocatable component/schema metadata.
        self.cpp_info.set_property("cmake_find_mode", "none")
        self.cpp_info.builddirs = ["lib/cmake/sqlconduit"]
        self.cpp_info.libs = ["sqlconduit_cored" if str(self.settings.build_type) == "Debug" else "sqlconduit_core"]
        if str(self.settings.os) in ("Linux", "FreeBSD"):
            self.cpp_info.system_libs = ["pthread"]
