caspar_add_test(image_tests
	SOURCES
		consumer_test.cpp
		producer_test.cpp
	LIBRARIES image ffmpeg core common
	LABELS module image
)
