# GoogleTest and the caspar_add_test() helper. Included when BUILD_TESTING is on.

# Prefer an installed GoogleTest (offline builds), otherwise fetch a pinned release.
find_package(GTest 1.14 CONFIG QUIET)
if (NOT GTest_FOUND)
	include(FetchContent)
	FetchContent_Declare(googletest
		URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
		URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
		DOWNLOAD_EXTRACT_TIMESTAMP ON
	)
	set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
	set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
	FetchContent_MakeAvailable(googletest)
endif ()

include(GoogleTest)

# caspar_add_test(<target> SOURCES <files...> [LIBRARIES <libs...>] [LABELS <labels...>])
#
# Builds a GoogleTest executable and registers each of its tests with CTest, tagged with LABELS
# (see AGENTS.md for the label conventions).
FUNCTION (caspar_add_test TARGET)
	cmake_parse_arguments(ARG "" "" "SOURCES;LIBRARIES;LABELS" ${ARGN})

	add_executable(${TARGET} ${ARG_SOURCES})
	set_target_properties(${TARGET} PROPERTIES
			CXX_STANDARD 20
			CXX_STANDARD_REQUIRED ON
			CXX_EXTENSIONS OFF
			FOLDER tests)
	target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR})
	target_include_directories(${TARGET} SYSTEM PRIVATE ${BOOST_INCLUDE_PATH})
	target_link_libraries(${TARGET} PRIVATE caspar_test_support ${ARG_LIBRARIES} casparcg_system_libraries TBB::tbb)

	# PRE_TEST lists the tests when ctest runs, not at build time, so a crashing or sanitizer
	# build still links.
	gtest_discover_tests(${TARGET}
		DISCOVERY_MODE PRE_TEST
		PROPERTIES LABELS "${ARG_LABELS}")
ENDFUNCTION ()
