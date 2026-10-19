add_executable(unit_tests main.cpp)
target_link_libraries(unit_tests PRIVATE GTest::gtest)

include(register.cmake)

target_link_libraries(unit_tests PRIVATE vmf::vmf)
target_sources(unit_tests PRIVATE contracts.cpp headers.c)

target_sources(unit_tests PRIVATE examples.cpp)
target_sources(unit_tests PRIVATE payload_contracts.cpp envelope_contracts.cpp)
target_sources(unit_tests PRIVATE stream_contracts.cpp)
target_sources(unit_tests PRIVATE managed_contracts.cpp)
target_sources(unit_tests PRIVATE managed_controls_contracts.cpp)
target_sources(unit_tests PRIVATE managed_state_contracts.cpp)
target_sources(unit_tests PRIVATE managed_consumer.c managed_consumer.cpp)
target_sources(unit_tests PRIVATE schema_contracts.cpp)

if(BUILD_SCHEMA_JSON)
	target_link_libraries(unit_tests PRIVATE vmf::schema_json)
	target_sources(unit_tests PRIVATE
		schema_json_contracts.cpp schema_json_headers.c)
endif()

if(BUILD_CLI)
	target_sources(unit_tests PRIVATE cli_contracts.cpp)
	target_link_libraries(unit_tests PRIVATE vmf_cli_support)
	target_include_directories(unit_tests PRIVATE "${PROJECT_SOURCE_DIR}/tools")
endif()

if(BUILD_HEADER_JSON)
	target_link_libraries(unit_tests PRIVATE vmf::header_json)
	target_sources(unit_tests PRIVATE header_json_contracts.cpp
		header_json_headers.c)
endif()

target_sources(unit_tests PRIVATE author_contracts.cpp author_headers.c
	author_consumer.cpp)
