caspar_add_test(ffmpeg_tests
	SOURCES
		producer_test.cpp
	LIBRARIES ffmpeg core common
	LABELS module ffmpeg
)
