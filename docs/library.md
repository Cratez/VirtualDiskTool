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
| `password` | Encrypts the volume. See [Encryption](#encryption). | Empty, so not encrypted |

ReFS is untested, and the other `FileSystem` values are for other platforms. Anything the platform can't do fails with `Errc::Unsupported`, and it never falls back to something else.

**`AttachOptions`**

| Field | Meaning | Default |
|---|---|---|
| `lifetime` | `Scoped` or `Persistent`. See [Lifetimes](#lifetimes). | `Scoped` |
| `readOnly` | Attach read-only. A read-only disk can't be formatted. | `false` |
| `mountPath` | A drive letter such as `Z:\`, or an empty folder on an NTFS volume. | The highest free drive letter |
| `password` | Unlocks an encrypted image. `Create` and `CreateTemporary` refuse it, because a new disk's password goes in `VolumeSpec`. | Empty |
| `recoveryKey` | Unlocks an encrypted image instead of `password`. `Create` and `CreateTemporary` refuse it too. | Empty |
| `allowLocked` | Attach an encrypted image without unlocking it. | `false` |

## Using a disk

```cpp
disk->GetMountPath();   // Where the volume is. Empty if nothing is mounted.
disk->GetImagePath();   // The image file.
disk->GetRecoveryKey(); // The recovery key, if this object encrypted the disk.
disk->IsLocked();       // True for an encrypted disk attached with allowLocked.
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

## Encryption

Setting `VolumeSpec::password` protects a volume with BitLocker, so a copy of the image file is unreadable without the password or recovery key. It works with `Create`, `CreateTemporary` and `Format`:

```cpp
auto disk = vdt::VirtualDisk::Create("C:\\Images\\secret.vhdx", {
	.size = vdt::GiB(1),
	.volume = { .password = "a long passphrase" } });
if (!disk) {
	std::cerr << disk.GetError().Message() << '\n';
	return 1;
}

// Save this somewhere other than next to the image.
std::ofstream("D:\\Keys\\secret.key") << disk->GetRecoveryKey();
```

Detaching locks the disk again. To open it, give `AttachOptions::password` or `AttachOptions::recoveryKey`:

```cpp
vdt::AttachOptions options;
options.password = "a long passphrase";

auto disk = vdt::VirtualDisk::Open("C:\\Images\\secret.vhdx", options);
if (!disk) {
	if (disk.GetError().code == vdt::Errc::WrongPassword) {
		std::cerr << "Wrong password.\n";
	}
	return 1;
}
```

Without either, `Open` fails with `Errc::Locked` and leaves the image detached. Giving either for an image that isn't encrypted fails with `Errc::InvalidArgument`, so that code can't wrongly believe a disk is protected.

`AttachOptions::allowLocked` attaches a locked image anyway. Its files stay unreadable, but `Format` can erase it, without the password. If the library can't tell whether an image is locked, `Open` fails with the reason. The exception is `allowLocked` with no password or key, which treats the image as locked.

**Save the recovery key.** `GetRecoveryKey()` is the only chance to get it through this library, and if the password and key are both lost, the data is gone for good. The command line tool requires a key file for this reason, but the library can't force you to keep it. Treat the key like the password, because anyone who has it can unlock the image.

### What it doesn't do

- **It only protects data at rest.** While a disk is attached and unlocked, anything on that machine with access to the volume can read it.
- **It doesn't stop anyone destroying data.** `Format` needs no password, and anyone with access to the file can delete it.
- **It can't be added to a disk that keeps its data.** Encrypting happens as part of creating or formatting, which start with an empty volume.
- **It can't change or remove a password.** To do either, copy the data off and `Format`.
- **It can't fully protect passwords in memory.** Passwords pass through ordinary `std::string` values. The library clears the copies it keeps, but not yours, and memory isn't locked against being paged out.

### Limits

- **Windows edition:** BitLocker needs Windows Pro, Enterprise or Education. Home editions can't create encrypted images.
- **Disk size:** a 64 MiB disk fails with `Errc::TooSmallToEncrypt`, and a 256 MiB disk works. The exact minimum isn't known yet.
- **File systems:** Microsoft documents this as NTFS only. exFAT worked in testing on Windows 10, but treat it as unofficial.
- **Passwords:** BitLocker can reject a short or simple password, and Group Policy can forbid passwords altogether.
- **Image size:** encrypting costs a roughly fixed amount of space in a new dynamic VHDX, rather than a share of its size. An encrypted VHDX used 196 MiB for a 256 MiB disk, 292 to 356 MiB for 1 GiB and 388 to 420 MiB for 8 GiB, varying between runs. A VHD uses less, at 52 MiB for a 256 MiB disk. `Format` encrypts the whole volume, and erases the rest of the disk, because old data could still be there. So it takes longer, and a dynamic image grows to about its full size.
- **Very large disks:** `Format` gives up after 12 hours with `Errc::Timeout`, which leaves the disk partly encrypted with no recovery key. Format it again to start over.
- **Older Windows:** images use Windows' default algorithm, XTS-AES 128, which Windows older than 10 version 1511 can't be expected to unlock.
- **Windows Sandbox:** encryption is untested there, and isn't expected to work, because Sandbox denied access to the WMI interfaces the tests tried, and BitLocker is managed through WMI. Unencrypted disks work there, apart from the full NTFS format issue below.

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
| `Locked` | The disk is encrypted, and no password or recovery key was given. |
| `WrongPassword` | The password or recovery key didn't unlock the disk. |
| `TooSmallToEncrypt` | The volume is too small for Windows to encrypt. |

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
- Encryption has [its own limits](#encryption).
