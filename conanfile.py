from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class BulutklinikSdkConan(ConanFile):
    name = "bulutklinik-sdk"
    version = "0.3.0"
    license = "MIT"
    url = "https://github.com/bulutklinik/cpp-sdk"
    homepage = "https://github.com/bulutklinik/cpp-sdk"
    description = "Official Bulutklinik API SDK for C++"
    topics = ("bulutklinik", "sdk", "api", "telemedicine", "health")

    settings = "os", "compiler", "build_type", "arch"
    options = {"shared": [True, False], "fPIC": [True, False]}
    default_options = {"shared": False, "fPIC": True}

    exports_sources = "CMakeLists.txt", "include/*", "src/*"

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def requirements(self):
        self.requires("cpr/1.10.5")
        self.requires("nlohmann_json/3.11.3")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.cache_variables["BULUTKLINIK_BUILD_TESTS"] = "OFF"
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.libs = ["bulutklinik_sdk"]
