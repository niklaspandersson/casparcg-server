# Unit tests for common, core and protocol, and the support library every test links.
# Module tests live next to their module.

casparcg_add_library(caspar_test_support SOURCES
	support/concurrency.cpp
	support/concurrency.h
	support/consumer_harness.cpp
	support/consumer_harness.h
	support/fake_frame_factory.cpp
	support/environment.cpp
	support/environment.h
	support/fake_frame_factory.h
	support/http_server.cpp
	support/http_server.h
	support/image.cpp
	support/image.h
	support/leak_check.cpp
	support/leak_check.h
	support/log_capture.cpp
	support/log_capture.h
	support/main.cpp
	support/output_dir.cpp
	support/output_dir.h
	support/process.cpp
	support/process.h
	support/producer_harness.cpp
	support/producer_harness.h
	support/runs.cpp
	support/runs.h
	support/test_media.cpp
	support/test_media.h
	support/watchdog.cpp
	support/watchdog.h
)
target_include_directories(caspar_test_support PUBLIC ${CMAKE_SOURCE_DIR}/tests/support)
target_include_directories(caspar_test_support PRIVATE ${CMAKE_SOURCE_DIR})
target_include_directories(caspar_test_support SYSTEM PRIVATE ${BOOST_INCLUDE_PATH} ${FFMPEG_INCLUDE_PATH})
target_link_libraries(caspar_test_support PUBLIC GTest::gmock PRIVATE common core)
set_target_properties(caspar_test_support PROPERTIES FOLDER tests)
if (CASPARCG_SANITIZE)
	target_compile_definitions(caspar_test_support PRIVATE CASPAR_TEST_SANITIZER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/sanitizers)
endif ()

caspar_add_test(support_tests
	SOURCES
		support/fake_frame_factory_test.cpp
		support/http_server_test.cpp
		support/log_capture_test.cpp
		support/output_dir_test.cpp
		support/producer_harness_test.cpp
		support/runs_test.cpp
		support/test_media_test.cpp
		support/watchdog_test.cpp
	LIBRARIES core common
	LABELS unit
)

caspar_add_test(common_tests
	SOURCES
		common/base64_test.cpp
		common/utf_test.cpp
	LIBRARIES common
	LABELS unit
)

caspar_add_test(protocol_tests
	SOURCES
		protocol/tokenize_test.cpp
	LIBRARIES protocol
	LABELS unit
)

# caspar_make_clip: stamped test clips for tests outside C++ (the end-to-end tests).
add_executable(caspar_make_clip tools/make_clip.cpp)
set_target_properties(caspar_make_clip PROPERTIES
		CXX_STANDARD 20
		CXX_STANDARD_REQUIRED ON
		CXX_EXTENSIONS OFF
		FOLDER tests)
target_include_directories(caspar_make_clip PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_include_directories(caspar_make_clip SYSTEM PRIVATE ${BOOST_INCLUDE_PATH})
target_link_libraries(caspar_make_clip PRIVATE caspar_test_support core common casparcg_system_libraries TBB::tbb)
if (NOT MSVC)
	target_link_options(caspar_make_clip PRIVATE LINKER:--no-as-needed)
endif ()

# End-to-end tests: pytest files in e2e/, run against the installed server (cmake --install). One
# CTest test per file; run single tests with pytest directly (see AGENTS.md).
find_package(Python3 COMPONENTS Interpreter)
if (Python3_FOUND)
	execute_process(COMMAND ${Python3_EXECUTABLE} -c "import pytest" RESULT_VARIABLE no_pytest OUTPUT_QUIET ERROR_QUIET)
endif ()
if (Python3_FOUND AND NOT no_pytest)
	# file name -> label; tests that start channels need an accelerator (a GPU, or Mesa's drivers)
	set(e2e_tests "lifecycle=e2e:accelerator" "amcp_tcp=e2e")
	foreach (entry ${e2e_tests})
		string(REPLACE "=" ";" entry "${entry}")
		list(GET entry 0 name)
		list(GET entry 1 label)
		add_test(NAME e2e.${name}
			COMMAND ${Python3_EXECUTABLE} -m pytest -p no:cacheprovider -v ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_${name}.py
			WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
		set_tests_properties(e2e.${name} PROPERTIES
			LABELS "${label}"
			TIMEOUT 900
			ENVIRONMENT "CASPAR_BINARY=${CMAKE_INSTALL_PREFIX}/bin/${CASPARCG_BINARY_NAME};CASPAR_MAKE_CLIP=$<TARGET_FILE:caspar_make_clip>")
	endforeach ()
else ()
	message(STATUS "pytest not found for ${Python3_EXECUTABLE}: end-to-end tests are not registered. "
		"Install pytest, or point -DPython3_EXECUTABLE at a Python that has it.")
endif ()
