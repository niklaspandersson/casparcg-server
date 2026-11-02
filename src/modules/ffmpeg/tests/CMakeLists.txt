caspar_add_test(ffmpeg_tests
	SOURCES
		consumer_test.cpp
		producer_race_test.cpp
		producer_teardown_test.cpp
		producer_test.cpp
	LIBRARIES ffmpeg core common
	LABELS module ffmpeg
)
