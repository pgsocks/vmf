set(CMAKE_CXX_STANDARD 17)
include("${PROJECT_SOURCE_DIR}/cmake/CPM.cmake")

if(BUILD_SCHEMA_JSON OR BUILD_HEADER_JSON)
	add_subdirectory(yyjson)
endif()

if(BUILD_TESTING AND (BUILD_UNIT_TESTS OR BUILD_ACCEPTANCE_TESTS))
	add_subdirectory(googletest EXCLUDE_FROM_ALL)
endif()

if(BUILD_TESTING AND BUILD_ACCEPTANCE_TESTS)
	add_subdirectory(nlohmann-json EXCLUDE_FROM_ALL)
	add_subdirectory(asio EXCLUDE_FROM_ALL)
	add_subdirectory(tclap EXCLUDE_FROM_ALL)
	add_subdirectory(cucumber-cpp EXCLUDE_FROM_ALL)
endif()
