add_library(vmf_cli_support STATIC header_json.cpp event_json.cpp)
target_link_libraries(vmf_cli_support PUBLIC vmf::schema_json yyjson)

add_executable(vmf_cli main.cpp)
set_target_properties(vmf_cli PROPERTIES OUTPUT_NAME vmf)
target_link_libraries(vmf_cli PRIVATE vmf_cli_support)
