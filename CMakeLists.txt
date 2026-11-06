cmake_minimum_required(VERSION 3.25)

# A project that pulls this in wants the library, not the tool and tests. This can't use
# PROJECT_IS_TOP_LEVEL, because the test option has to be known before project().
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	set(VDT_TOP_LEVEL ON)
else()
	set(VDT_TOP_LEVEL OFF)
endif()

option(VDT_BUILD_TOOL "Build the command line tool." ${VDT_TOP_LEVEL})
option(VDT_BUILD_TESTS "Build the test suite, which needs GoogleTest from vcpkg." ${VDT_TOP_LEVEL})
if(VDT_BUILD_TESTS)
	# Has to come before project(), which is when vcpkg installs what vcpkg.json asks for.
	list(APPEND VCPKG_MANIFEST_FEATURES tests)
endif()

project(VirtualDiskTool VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
	# Static CRT so the tool runs on machines without the VC++ redistributable. A parent
	# project keeps its own choice, because everything it links has to agree on one.
	if(VDT_TOP_LEVEL)
		set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	endif()
	add_compile_options(/W4 /permissive- /utf-8)
endif()

if(WIN32)
	# The toolset decides the Windows API level: the standard library in v145
	# (MSVC 19.50+) requires Windows 10. v143 builds stay at the Windows 7 level so
	# that Windows 8+ declarations are hidden. This is a guard, not a support claim.
	if(MSVC_VERSION GREATER_EQUAL 1950)
		set(VDT_WIN32_WINNT 0x0A00)
	else()
		set(VDT_WIN32_WINNT 0x0601)
	endif()
	add_compile_definitions(
		_WIN32_WINNT=${VDT_WIN32_WINNT}
		WINVER=${VDT_WIN32_WINNT}
		UNICODE
		_UNICODE
		NOMINMAX
		WIN32_LEAN_AND_MEAN)
endif()

add_library(VirtualDiskLib STATIC
	src/BuildInfo.cpp
	src/Errors.cpp
	src/VirtualDisk.cpp)
target_include_directories(VirtualDiskLib PUBLIC include PRIVATE src)
target_compile_features(VirtualDiskLib PUBLIC cxx_std_20)
add_library(vdt::VirtualDisk ALIAS VirtualDiskLib)

if(WIN32)
	target_sources(VirtualDiskLib PRIVATE
		src/windows/BitLocker.cpp
		src/windows/DiskLayout.cpp
		src/windows/VolumeFormat.cpp
		src/windows/WindowsBackend.cpp)
	target_link_libraries(VirtualDiskLib PRIVATE virtdisk ole32)
else()
	message(FATAL_ERROR "There is no backend for this platform yet. See DESIGN.md.")
endif()

if(VDT_BUILD_TOOL OR VDT_BUILD_TESTS)
	add_library(VirtualDiskCli STATIC
		tools/Cli.cpp)
	target_include_directories(VirtualDiskCli PUBLIC tools)
	target_link_libraries(VirtualDiskCli PUBLIC VirtualDiskLib)
	target_compile_definitions(VirtualDiskCli PRIVATE VDT_VERSION="${PROJECT_VERSION}")
endif()

if(VDT_BUILD_TOOL)
	add_executable(VirtualDiskTool
		tools/main.cpp)
	target_link_libraries(VirtualDiskTool PRIVATE VirtualDiskCli)
endif()

if(VDT_BUILD_TESTS)
	find_package(GTest CONFIG REQUIRED)

	add_executable(VirtualDiskTests
		tests/main.cpp
		tests/CliTests.cpp
		tests/DocumentationExamples.cpp
		tests/IntegrationTests.cpp
		tests/ResultTests.cpp
		tests/VirtualDiskTests.cpp
		tests/WindowsTests.cpp)
	target_include_directories(VirtualDiskTests PRIVATE src tests)
	target_link_libraries(VirtualDiskTests PRIVATE VirtualDiskCli GTest::gtest)

	# Keeps the two files that go to the test machine together. This is a target of its own
	# because a post-build step only runs on a relink, and wouldn't notice the script changing.
	add_custom_target(CopyTestRunner ALL
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			${CMAKE_CURRENT_SOURCE_DIR}/tests/Run-Tests.ps1
			$<TARGET_FILE_DIR:VirtualDiskTests>)
	add_dependencies(CopyTestRunner VirtualDiskTests)
endif()
