from conan import ConanFile


required_conan_version = ">=2.0"


class JshookzCppConan(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def build_requirements(self):
        self.test_requires("gtest/1.17.0")
