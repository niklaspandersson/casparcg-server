cmake_minimum_required (VERSION 3.28)
project("modules")

add_subdirectory(image)
add_subdirectory(ffmpeg)
add_subdirectory(oal)
add_subdirectory(decklink)
add_subdirectory(screen)
add_subdirectory(newtek)

# Artnet has boost::variant compatibility issues on macOS
if (NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	add_subdirectory(artnet)
endif()

if (ENABLE_HTML)
	add_subdirectory(html)
endif ()

if (MSVC)
	add_subdirectory(flash)
	add_subdirectory(bluefish)
endif()

