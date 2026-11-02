# Unit tests for common, core and protocol, and the support library every test links.
# Module tests live next to their module.

casparcg_add_library(caspar_test_support SOURCES
	support/fake_frame_factory.cpp
	support/environment.cpp
	support/environment.h
	support/fake_frame_factory.h
	support/http_server.cpp
	support/http_server.h
	support/log_capture.cpp
	support/log_capture.h
	support/main.cpp
	support/output_dir.cpp
	support/output_dir.h
	support/producer_harness.cpp
	support/producer_harness.h
	support/runs.cpp
	support/runs.h
	support/test_media.cpp
	support/test_media.h
)
target_include_directories(caspar_test_support PUBLIC ${CMAKE_SOURCE_DIR}/tests/support)
target_include_directories(caspar_test_support PRIVATE ${CMAKE_SOURCE_DIR})
target_include_directories(caspar_test_support SYSTEM PRIVATE ${BOOST_INCLUDE_PATH} ${FFMPEG_INCLUDE_PATH})
target_link_libraries(caspar_test_support PUBLIC GTest::gmock PRIVATE common core)
set_target_properties(caspar_test_support PROPERTIES FOLDER tests)

caspar_add_test(support_tests
	SOURCES
		support/fake_frame_factory_test.cpp
		support/http_server_test.cpp
		support/log_capture_test.cpp
		support/output_dir_test.cpp
		support/runs_test.cpp
		support/test_media_test.cpp
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
