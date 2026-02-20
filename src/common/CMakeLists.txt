cmake_minimum_required (VERSION 3.28)
project (common)

set(SOURCES
		diagnostics/graph.cpp

		base64.cpp
		env.cpp
		filesystem.cpp
		log.cpp
		tweener.cpp
		utf.cpp
)

# Platform-specific sources
if (MSVC)
	list(APPEND SOURCES
			gl/gl_check.cpp

			compiler/vs/disable_silly_warnings.h

			os/windows/filesystem.cpp
			os/windows/prec_timer.cpp
			os/windows/process.cpp
			os/windows/thread.cpp
			os/windows/windows.h
	)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	list(APPEND SOURCES
			gl/gl_check_stub.cpp

			os/macos/env_macos.cpp
			os/macos/filesystem.cpp
			os/macos/prec_timer.cpp
			os/macos/thread.cpp
	)
else ()
	list(APPEND SOURCES
			gl/gl_check.cpp

			os/linux/filesystem.cpp
			os/linux/prec_timer.cpp
			os/linux/thread.cpp
	)
endif ()

set(HEADERS
		diagnostics/graph.h

		gl/gl_check.h

		os/filesystem.h
		os/thread.h

		array.h
		assert.h
		base64.h
		endian.h
		enum_class.h
		env.h
		executor.h
		except.h
		filesystem.h
		future.h
		log.h
		memory.h
		memshfl.h
		param.h
		prec_timer.h
		ptree.h
		scope_exit.h
		stdafx.h
		timer.h
		timespan.h
		tweener.h
		utf.h
)

casparcg_add_library(common SOURCES ${SOURCES} ${HEADERS})
target_include_directories(common PRIVATE ..)
target_precompile_headers(common PRIVATE stdafx.h)

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	find_library(COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)
	target_link_libraries(common PRIVATE ${COREFOUNDATION_LIBRARY})
else ()
	target_link_libraries(common PRIVATE GLEW::glew)
endif ()

source_group(sources ./*)
source_group(sources\\gl gl/*)
source_group(sources\\diagnostics diagnostics/*)
source_group(sources\\compiler\\vs compiler/vs/*)
source_group(sources\\os\\windows os/windows/*)
source_group(sources\\os\\linux os/linux/*)
source_group(sources\\os\\macos os/macos/*)
source_group(sources\\os os/*)
