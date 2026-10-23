function(run_checked)
	execute_process(COMMAND ${ARGV} RESULT_VARIABLE result
		OUTPUT_VARIABLE output ERROR_VARIABLE error)
	if(NOT result EQUAL 0)
		message(FATAL_ERROR "${ARGV} failed:\n${output}${error}")
	endif()
endfunction()

set(config_args)
if(CONFIGURATION)
	list(APPEND config_args --config "${CONFIGURATION}")
endif()

set(scratch "${BINARY_DIR}/installation-regression")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}/original")
run_checked("${CMAKE_COMMAND}" --install "${BINARY_DIR}"
	${config_args} --prefix "${scratch}/original")
file(RENAME "${scratch}/original" "${scratch}/relocated")
set(prefix "${scratch}/relocated")

set(disable_yyjson OFF)
if(NOT SCHEMA_JSON AND NOT HEADER_JSON)
	set(disable_yyjson ON)
endif()

foreach(language c cxx)
	set(consumer "${scratch}/${language}")
	run_checked("${CMAKE_COMMAND}"
		-S "${SOURCE_DIR}/tests/infrastructure/install-${language}"
		-B "${consumer}" -G "${GENERATOR}"
		"-DCMAKE_PREFIX_PATH=${prefix}"
		"-DCMAKE_FIND_ROOT_PATH=${prefix}"
		-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY
		-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF
		-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
		"-DTEST_SCHEMA_JSON=${SCHEMA_JSON}"
		"-DTEST_HEADER_JSON=${HEADER_JSON}"
		"-DCMAKE_DISABLE_FIND_PACKAGE_yyjson=${disable_yyjson}")
	run_checked("${CMAKE_COMMAND}" --build "${consumer}"
		${config_args})
	set(executable "${consumer}/consumer${EXECUTABLE_SUFFIX}")
	if(CONFIGURATION AND EXISTS
		"${consumer}/${CONFIGURATION}/consumer${EXECUTABLE_SUFFIX}")
		set(executable
			"${consumer}/${CONFIGURATION}/consumer${EXECUTABLE_SUFFIX}")
	endif()
	run_checked("${executable}")
endforeach()

if(BUILD_CLI)
	run_checked("${prefix}/${INSTALL_BINDIR}/${PACKAGE}${EXECUTABLE_SUFFIX}"
		--help)
endif()

set(missing_components unknown)
if(NOT SCHEMA_JSON)
	list(APPEND missing_components schema_json)
endif()
if(NOT HEADER_JSON)
	list(APPEND missing_components header_json)
endif()
foreach(component IN LISTS missing_components)
	set(source "${scratch}/missing-${component}")
	file(MAKE_DIRECTORY "${source}")
	file(WRITE "${source}/CMakeLists.txt"
		"cmake_minimum_required(VERSION 3.24)\n"
		"project(missing_component LANGUAGES C)\n"
		"find_package(vmf REQUIRED CONFIG COMPONENTS ${component})\n")
	execute_process(COMMAND "${CMAKE_COMMAND}" -S "${source}"
		-B "${source}/build" "-DCMAKE_PREFIX_PATH=${prefix}"
		"-DCMAKE_FIND_ROOT_PATH=${prefix}"
		-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY
		-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF
		-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
		RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
	if(result EQUAL 0)
		message(FATAL_ERROR "Unavailable component ${component} was accepted")
	endif()
endforeach()
