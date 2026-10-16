set(CMAKE_CXX_STANDARD 17)
include("${PROJECT_SOURCE_DIR}/cmake/CPM.cmake")

if(BUILD_SCHEMA_JSON)
	add_subdirectory(yyjson)
endif()

if(BUILD_TESTING AND (BUILD_UNIT_TESTS OR BUILD_ACCEPTANCE_TESTS))
	add_subdirectory(googletest)
endif()

if(BUILD_TESTING AND BUILD_ACCEPTANCE_TESTS)
	add_subdirectory(nlohmann-json)
	add_subdirectory(asio)
	add_subdirectory(tclap)
	add_subdirectory(cucumber-cpp)
endif()
