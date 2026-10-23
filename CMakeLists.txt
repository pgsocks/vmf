cmake_minimum_required(VERSION 3.24)
project(native_repo_template LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)
option(BUILD_UNIT_TESTS "Build GoogleTest unit tests" ON)
option(BUILD_ACCEPTANCE_TESTS "Build Cucumber acceptance tests" OFF)
option(BUILD_SCHEMA_JSON "Build the JSON payload schema library" ON)
option(BUILD_HEADER_JSON "Build the JSON envelope header library" ON)
include(CMakeDependentOption)
cmake_dependent_option(BUILD_CLI "Build the VMF command-line tool" ON
	"BUILD_SCHEMA_JSON;BUILD_HEADER_JSON" OFF)

if(BUILD_TESTING AND BUILD_ACCEPTANCE_TESTS)
	include(cmake/AcceptancePrerequisites.cmake)
endif()
if(BUILD_SCHEMA_JSON OR BUILD_HEADER_JSON OR (BUILD_TESTING AND
	(BUILD_UNIT_TESTS OR BUILD_ACCEPTANCE_TESTS)))
	add_subdirectory(cmake/dependencies)
endif()

add_subdirectory(src)
if(BUILD_CLI)
	add_subdirectory(tools)
endif()

if(BUILD_TESTING AND (BUILD_UNIT_TESTS OR BUILD_ACCEPTANCE_TESTS))
	add_subdirectory(tests)
endif()

configure_package_config_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/cmake/vmf-config.cmake.in"
	"${CMAKE_CURRENT_BINARY_DIR}/vmf-config.cmake"
	INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vmf"
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/vmf-config.cmake"
	DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vmf")
install(EXPORT vmf-targets NAMESPACE vmf::
	DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vmf")
install(DIRECTORY include/vmf
	DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
