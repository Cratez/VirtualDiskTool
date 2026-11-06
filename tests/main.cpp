#include <gtest/gtest.h>

#include <filesystem>
#include <string_view>

int RunCrashChild(const std::filesystem::path& image);

int wmain(int argc, wchar_t* argv[]) {
	// See AScopedAttachmentEndsWhenTheProcessDies.
	if (argc == 3 && std::wstring_view(argv[1]) == L"--crash-child") {
		return RunCrashChild(argv[2]);
	}

	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
