# Using the library

The library gives C++ code a mounted virtual disk in a few lines, and cleans up after itself. A common use is testing code against a real file system:

```cpp
#include <fstream>
#include <iostream>
#include <vdt/VirtualDisk.h>

int main() {
	auto disk = vdt::VirtualDisk::CreateTemporary({ .size = vdt::MiB(64) });
	if (!disk) {
		std::cerr << disk.GetError().Message() << '\n';
		return 1;
	}

	std::ofstream(disk->GetMountPath() / "hello.txt") << "hi";
	return 0;
	// Leaving scope unmounts, detaches and deletes the image.
}
```

## Requirements

- Windows, and Visual Studio 2022 or 2026.
- C++20.
- Admin rights at run time for anything that attaches a disk. Run your program elevated, or set its UAC execution level to `requireAdministrator`.

## Adding it to your project

### With CMake

This is the simplest way, because CMake handles the settings for you. Pin `GIT_TAG` to the commit you want:

```cmake
include(FetchContent)
FetchContent_Declare(VirtualDiskTool
	GIT_REPOSITORY https://github.com/Cratez/VirtualDiskTool.git
	GIT_TAG <commit>)
FetchContent_MakeAvailable(VirtualDiskTool)

target_link_libraries(MyProgram PRIVATE vdt::VirtualDisk)
```

If you keep a copy in your repository, such as a git submodule, replace those first lines with `add_subdirectory(external/VirtualDiskTool)`.

Used this way, the library:

- Builds only itself, not the tool or tests, so it needs neither vcpkg nor GoogleTest.
- Uses your project's toolset and C runtime, so `/MD` and `/MT` both work.
- Turns on C++20 for targets that link it.

To build the tool as well, set `VDT_BUILD_TOOL` to `ON` before `FetchContent_MakeAvailable`.

### With a Visual Studio project

1. Build the library with the preset that matches your project's toolset. See [Building](../README.md#building). Use `vs2022` for v143 and `vs2026` for v145, because a library can't reliably be linked by an older compiler than the one that built it.
2. Set these in your project's properties:

| Property | Value |
|---|---|
| C/C++ > General > Additional Include Directories | `<repo>\include` |
| C/C++ > Language > C++ Language Standard | ISO C++20 or later |
| C/C++ > Code Generation > Runtime Library | `/MT` for Release, `/MTd` for Debug |
| Linker > Input > Additional Dependencies | `<repo>\build\<preset>\<config>\VirtualDiskLib.lib;virtdisk.lib;ole32.lib` |

The runtime library has to match, because the presets build with the static runtime. If you need `/MD`, use the CMake route above.

## Getting a disk

| Function | What it does | When it ends |
|---|---|---|
| `VirtualDisk::CreateTemporary(spec)` | Creates an image at a unique temporary path, then attaches, formats and mounts it. | Detached and deleted with the object |
| `VirtualDisk::Create(path, spec)` | The same at a path you choose. Never overwrites. | Detached with the object, and the image stays |
| `VirtualDisk::Open(path)` | Attaches an existing image and mounts its volume, if it has one. | Detached with the object, and the image stays |
| `CreateBlankImage(path, spec)` | Only creates the file. Usually needs no admin rights. | Not attached |

Each of these returns a `Result`, so check it before use. `Create` and `CreateTemporary` are all or nothing: if a step fails, no image is left behind.

`CreateTemporary`, `Create` and `Open` also take optional `AttachOptions`, as their next argument after the spec or path:

```cpp
vdt::AttachOptions options;
options.mountPath = std::filesystem::path("Z:\\");

auto disk = vdt::VirtualDisk::Create("C:\\Images\\data.vhdx", { .size = vdt::GiB(1) }, options);
if (!disk) {
	std::cerr << disk.GetError().Message() << '\n';
	return 1;
}

// ... use Z:\ ...

auto detached = disk->Detach();
if (!detached) {
	std::cerr << detached.GetError().Message() << '\n';
	return 1;
}
```

## Choosing options

Only `size` is required, so you name just what you care about:

```cpp
auto disk = vdt::VirtualDisk::CreateTemporary({
	.size = vdt::GiB(1),
	.format = vdt::ImageFormat::Vhdx,
	.allocation = vdt::Allocation::Fixed,
	.volume = {
		.fileSystem = vdt::FileSystem::ExFat,
		.partitionStyle = vdt::PartitionStyle::Mbr,
		.label = "Scratch",
		.quickFormat = false } });
```

**`DiskSpec`**

| Field | Meaning | Default |
|---|---|---|
| `size` | Size in bytes. `vdt::MiB()` and `vdt::GiB()` help. Windows rejects under 3 MiB. | Required |
| `format` | `Auto`, `Vhd` or `Vhdx`. `Auto` follows the file extension. | `Auto` |
| `allocation` | `Dynamic` or `Fixed`. | `Dynamic` |
| `volume` | A `VolumeSpec`. | See below |

The file extension has to match the format, so `.vhd` for `Vhd` and `.vhdx` for `Vhdx`. `Raw` is reserved for other platforms.

**`VolumeSpec`**

| Field | Meaning | Default |
|---|---|---|
| `fileSystem` | `Default`, `None`, `Ntfs`, `ExFat`, `Fat32`, `Fat`, `Udf` or `ReFs`. `Default` is NTFS on Windows, and `None` leaves the disk blank and unmounted. | `Default` |
| `partitionStyle` | `Gpt` or `Mbr`. | `Gpt` |
| `label` | Volume label, in UTF-8. | Empty |
| `quickFormat` | `false` for a full format. | `true` |

ReFS is untested, and the other `FileSystem` values are for other platforms. Anything the platform can't do fails with `Errc::Unsupported`, and it never falls back to something else.

**`AttachOptions`**

| Field | Meaning | Default |
|---|---|---|
| `lifetime` | `Scoped` or `Persistent`. See [Lifetimes](#lifetimes). | `Scoped` |
| `readOnly` | Attach read-only. A read-only disk can't be formatted. | `false` |
| `mountPath` | A drive letter such as `Z:\`, or an empty folder on an NTFS volume. | The highest free drive letter |

## Using a disk

```cpp
disk->GetMountPath();   // Where the volume is. Empty if nothing is mounted.
disk->GetImagePath();   // The image file.
disk->Format(volume);   // Erases the disk and writes a new volume.
disk->Detach();         // Detaches now, and reports any failure.
disk->Release();        // Persistent only: stop managing it, and leave it attached.
```

`Format` takes a `VolumeSpec`, so changing file system looks like this:

```cpp
auto formatted = disk.Format({ .fileSystem = vdt::FileSystem::Fat32, .label = "Fresh" });
```

`VirtualDisk` is move-only. After it is moved from, detached or released, `GetImagePath` still works, and other calls fail with `Errc::NotAttached`.

An instance isn't thread-safe, but separate instances can be used at the same time.

## Lifetimes

**`Lifetime::Scoped`**, the default, ends with the object. On Windows it also ends if your process crashes, because Windows detaches the disk when the handle closes. A crash can still leave the image file behind.

**`Lifetime::Persistent`** survives your process, until something detaches it:

```cpp
vdt::AttachOptions options;
options.lifetime = vdt::Lifetime::Persistent;

auto disk = vdt::VirtualDisk::Open("C:\\Images\\data.vhdx", options);
if (!disk) {
	return 1;
}
auto released = disk->Release();
if (!released) {
	return 1;
}

// Later, possibly from another process:
auto detached = vdt::DetachImage("C:\\Images\\data.vhdx");
```

A destructor can't report failure. If you need to know that a detach worked, call `Detach()` yourself.

## Finding attached disks

`ListAttached` returns every virtual disk Windows has attached, including ones this library didn't attach:

```cpp
auto attached = vdt::ListAttached();
if (!attached) {
	return 1;
}
for (const vdt::AttachedImage& image : *attached) {
	std::wcout << image.image.wstring() << L" is " << image.device.wstring() << L'\n';
}
```

Without admin rights, `device` and `mountPath` are empty.

## Handling errors

The library doesn't throw, apart from `std::bad_alloc` when memory runs out. Anything that can fail returns a `Result<T>`, which holds a value or an `Error`:

```cpp
auto disk = vdt::VirtualDisk::Open("C:\\Images\\data.vhdx");
if (!disk) {
	const vdt::Error& error = disk.GetError();
	if (error.code == std::errc::permission_denied) {
		std::cerr << "Run this as administrator.\n";
	}
	else if (error.code == vdt::Errc::AlreadyAttached) {
		std::cerr << "Detach it first.\n";
	}
	else {
		std::cerr << error.Message() << '\n';
	}
	return 1;
}
```

Check a `Result` before reading it: reading the value of a failed one, or the error of a successful one, aborts the process.

`Error::code` is a `std::error_code`. Failures from the OS keep their native code, and can usually be compared against `std::errc`. These mean the same on every platform:

| `vdt::Errc` | Meaning |
|---|---|
| `InvalidArgument` | A spec, option or path failed validation. |
| `Unsupported` | This platform can't do what was asked. |
| `AlreadyExists` | Refused to overwrite an existing image. |
| `NotAttached` | The image isn't attached. |
| `AlreadyAttached` | Refused: the image is attached already. |
| `NotVirtualDisk` | Refused: Windows didn't confirm that the target is a file-backed virtual disk. |
| `NoVolume` | The disk has no volume to mount. |
| `Timeout` | Windows didn't bring the disk or volume online in time. |

## Safety

The library can only destroy data on a disk it attached itself. There is no way to format by drive letter or device path. Before any destructive step, it has Windows confirm that the target is a file-backed virtual disk, and stops with `Errc::NotVirtualDisk` if the answer is no or unclear.

An image that is already attached can't be opened again, so a disk someone has mounted can't be reached and erased through a second object.

## Testing your own code

`CreateTemporary`, `Create`, `Open`, `CreateBlankImage`, `DetachImage` and `ListAttached` all take an optional `vdt::IBackend&` as their last argument. Implement that interface from `vdt/Backend.h` to test code that uses the library, without admin rights or real disks. `tests/FakeBackend.h` in this repository is a complete example.

The same interface is how support for another platform would be added. See [DESIGN.md](../DESIGN.md).

## Known limits

- A disk holds exactly one volume.
- Windows 10 is the only supported version. See [Supported platforms](../README.md#supported-platforms).
- A full NTFS format failed in Windows Sandbox on Windows 11. Quick formats work there.
- ReFS is untested.
