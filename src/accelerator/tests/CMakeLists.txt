caspar_add_test(accelerator_tests SOURCES compositor_harness.cpp compositor_harness.h compositor_test.cpp LIBRARIES
                    accelerator core common LABELS compositor                         accelerator)
    target_include_directories(accelerator_tests SYSTEM PRIVATE ${FFMPEG_INCLUDE_PATH})
