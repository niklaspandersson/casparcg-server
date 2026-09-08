cmake_minimum_required (VERSION 3.28)
project (oal)

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # macOS: Use Core Audio consumer
    set(SOURCES
        consumer/coreaudio_consumer.mm
        consumer/coreaudio_consumer.h

        oal.cpp
        oal.h
    )

    find_library(AUDIOTOOLBOX_LIBRARY AudioToolbox)
    find_library(COREAUDIO_LIBRARY CoreAudio)

    casparcg_add_module_project(oal
        SOURCES ${SOURCES}
        INIT_FUNCTION "oal::init"
    )
    target_include_directories(oal PRIVATE
        ${FFMPEG_INCLUDE_PATH}
    )
    target_link_libraries(oal PRIVATE
        ${AUDIOTOOLBOX_LIBRARY}
        ${COREAUDIO_LIBRARY}
        Apple::Frameworks
    )
else()
    # Windows/Linux: Use OpenAL consumer
    set(SOURCES
        consumer/oal_consumer.cpp
        consumer/oal_consumer.h

        oal.cpp
        oal.h
    )

    casparcg_add_module_project(oal
        SOURCES ${SOURCES}
        INIT_FUNCTION "oal::init"
    )
    target_include_directories(oal PRIVATE
        ${FFMPEG_INCLUDE_PATH}
    )
    target_link_libraries(oal PRIVATE OpenAL::OpenAL)
endif()

set_target_properties(oal PROPERTIES FOLDER modules)
source_group(sources\\consumer consumer/*)
source_group(sources ./*)
