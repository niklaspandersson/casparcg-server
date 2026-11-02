# Unit tests for common, core and protocol. Module tests live next to their module.

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
