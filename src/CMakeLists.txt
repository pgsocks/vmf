add_library(vmf STATIC
	describe.cpp allocation.cpp schema.cpp envelope.cpp bits.cpp
	traversal.cpp adapters.cpp payload.cpp payload_bridge.cpp
	runtime_schema.cpp author.cpp
)
add_library(vmf::vmf ALIAS vmf)
target_include_directories(vmf PUBLIC
	$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
	$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(vmf PUBLIC c_std_11 cxx_std_17)

if(BUILD_SCHEMA_JSON)
	add_library(vmf_schema_json STATIC schema_json.cpp)
	add_library(vmf::schema_json ALIAS vmf_schema_json)
	target_link_libraries(vmf_schema_json PUBLIC vmf::vmf PRIVATE yyjson)
	target_compile_features(vmf_schema_json PUBLIC c_std_11 cxx_std_17)
endif()

if(BUILD_HEADER_JSON)
	add_library(vmf_header_json STATIC header_json.cpp)
	add_library(vmf::header_json ALIAS vmf_header_json)
	target_link_libraries(vmf_header_json PUBLIC vmf::vmf PRIVATE yyjson)
	target_compile_features(vmf_header_json PUBLIC c_std_11 cxx_std_17)
endif()

install(TARGETS vmf EXPORT vmf-targets
	ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
if(BUILD_SCHEMA_JSON)
	set_target_properties(vmf_schema_json PROPERTIES EXPORT_NAME schema_json)
	install(TARGETS vmf_schema_json EXPORT vmf-targets
		ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endif()
if(BUILD_HEADER_JSON)
	set_target_properties(vmf_header_json PROPERTIES EXPORT_NAME header_json)
	install(TARGETS vmf_header_json EXPORT vmf-targets
		ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endif()
