# The upstream cucumber-cpp library supplies the native wire-server main.
add_executable(acceptance_steps steps.cpp payload_steps.cpp author_steps.cpp)
target_sources(acceptance_steps PRIVATE managed_steps.cpp
	../unit/managed_consumer.c ../unit/managed_consumer.cpp)
target_link_libraries(acceptance_steps PRIVATE cucumber-cpp GTest::gtest)

if(BUILD_SCHEMA_JSON)
	target_sources(acceptance_steps PRIVATE schema_json_steps.cpp)
	target_link_libraries(acceptance_steps PRIVATE vmf::schema_json)
endif()

if(BUILD_HEADER_JSON)
	target_sources(acceptance_steps PRIVATE header_json_steps.cpp)
	target_link_libraries(acceptance_steps PRIVATE vmf::header_json)
endif()

set(acceptance_arguments "${PROJECT_SOURCE_DIR}/features")
if(NOT BUILD_HEADER_JSON)
	list(APPEND acceptance_arguments --tags "not @header_json")
endif()
if(NOT BUILD_SCHEMA_JSON)
	list(APPEND acceptance_arguments --tags "not @schema_json")
endif()
if(BUILD_CLI)
	target_sources(acceptance_steps PRIVATE cli_steps.cpp)
	add_dependencies(acceptance_steps vmf_cli)
else()
	list(APPEND acceptance_arguments --tags "not @cli")
endif()

add_test(NAME acceptance
	COMMAND "${RUBY_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run.rb"
		$<TARGET_FILE:acceptance_steps> ${acceptance_arguments}
)
set_tests_properties(acceptance PROPERTIES
	LABELS acceptance
	WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)
if(BUILD_CLI)
	set_property(TEST acceptance PROPERTY ENVIRONMENT
		"VMF_CLI_EXECUTABLE=$<TARGET_FILE:vmf_cli>"
		"VMF_CLI_RUBY=${RUBY_EXECUTABLE}"
		"VMF_CLI_DRIVER=${CMAKE_CURRENT_SOURCE_DIR}/cli_process.rb")
endif()

target_link_libraries(acceptance_steps PRIVATE vmf::vmf)
