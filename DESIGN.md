# VirtualDiskTool design

**Status:** the library, Windows backend, command line tool and test suite are written. The test suite passes on Windows 10 22H2 (build 19045) in a Hyper-V VM. That run used the `vs2026` build, and the `vs2022` build last passed before the command line tool's real-disk tests were added.

ReFS has never been tested. `FormatEx` reports it as unavailable on Windows 10 Pro, and it failed in Windows Sandbox, so its test has only ever been skipped. Testing it needs a Windows edition that can create it.

In Windows Sandbox on Windows 11 (build 26100), a full (not quick) NTFS format fails at every size tested, before any progress is reported. The same binary passes on Windows 10, so the code isn't obviously at fault, but it isn't known yet whether the cause is Sandbox or Windows 11 itself.

## Goals

1. **Easy to use.** The common case is a few lines, with nothing to clean up.
2. **Well defined.** Every operation has a stated result, including when it fails.
3. **Safe.** The library can only destroy data on a virtual disk it attached itself.
4. **Portable.** No OS type appears in the public API, so other platforms are a new backend and not a redesign.

## Usage

```cpp
auto disk = vdt::VirtualDisk::CreateTemporary({ .size = vdt::MiB(64) });
if (!disk) {
	std::cerr << disk.GetError().Message() << '\n';
	return 1;
}
std::ofstream(disk->GetMountPath() / "hello.txt") << "hi";
// Leaving scope unmounts, detaches and deletes the image.
```

Specs have defaults for everything except size, so callers only name what they care about:

```cpp
auto disk = vdt::VirtualDisk::CreateTemporary({
	.size = vdt::GiB(1),
	.format = vdt::ImageFormat::Vhdx,
	.volume = { .fileSystem = vdt::FileSystem::ExFat, .label = "Scratch" } });
```

## Layers

| Layer | Job |
|---|---|
| `VirtualDiskTool` command line tool | Parses arguments and calls the library. Holds no disk logic. |
| `VirtualDisk` | Ownership, lifecycle and safety checks. Contains no OS code, so every backend gets them. |
| `IBackend`, `IAttachment` | The contract each platform implements. |
| Backends | Windows first. POSIX backends later. A fake backend for tests. |

Other projects link the `vdt::VirtualDisk` target, as [docs/library.md](docs/library.md) describes. Pulled in as a subproject, this repository builds only the library and leaves the C runtime to the parent project, because everything linked together has to agree on one.

## Rules

**Ownership**

- A `VirtualDisk` always holds a live attachment until it is moved from, detached or released.
- `Lifetime::Scoped` is the default and ends with the object. On Windows this uses a non-permanent attach, so the OS also cleans up if the process crashes.
- `Lifetime::Persistent` outlives the process. It exists for the command line tool and for scripts.
- Destructors can't report failure. Call `Detach` to see errors.

**Errors**

- The library doesn't throw, apart from `std::bad_alloc` when memory runs out. Everything that can fail returns `Result<T>`, which must be checked.
- An `Error` holds a `std::error_code` and the step that failed.
- OS errors keep their native code but still compare portably, for example against `std::errc::permission_denied`.
- Conditions that mean the same everywhere use `vdt::Errc`.

**Safety**

- Nothing formats by drive letter or device path. The only destructive path is `VirtualDisk::Format` or creating a new disk, on a disk the object attached itself.
- Before any destructive call, `VirtualDisk` asks the OS to confirm the target is a file-backed virtual disk. On Windows that is `BusTypeFileBackedVirtual`. If the answer is no or unknown, it stops with `Errc::NotVirtualDisk`.
- The Windows backend repeats that check on the same handle it writes through, so the target can't change between the check and the write.
- An attached image can't be opened again, which fails with `Errc::AlreadyAttached`. So `format` can't erase a disk someone has mounted. This is checked explicitly, because what Windows allows can depend on whether the disk has been written to.
- Images are never overwritten. Creating over an existing file fails with `Errc::AlreadyExists`.
- Creating is all or nothing. If any step fails, no image is left behind.
- Unknown or unavailable options fail with `Errc::Unsupported`. Nothing falls back to a different file system or format.

**Scope limit:** a disk holds exactly one volume. Multiple partitions can be added later if a real need appears.

## Platforms

Only the Windows column is planned for now. The others record how the contract is expected to map, so that it stays implementable.

| Concept | Windows | Linux | macOS |
|---|---|---|---|
| Image formats | VHD, VHDX | Raw | Raw |
| Attach | virtdisk API | Loop device | `hdiutil attach` |
| OS cleanup after a crash | Yes | Yes, loop autoclear | No, reported by `CleansUpAfterCrash` |
| Partition | Disk IOCTLs | `sfdisk` | `diskutil` |
| Format | `fmifs.dll` `FormatEx` | `mkfs.*` | `newfs_*` |
| Virtual disk check | `BusTypeFileBackedVirtual` | Device is a loop device | `hdiutil info` |

## Windows versions

Windows 10 is the only supported version, because it is the only one tested. We don't claim Windows 7, but we avoid what we know breaks it:

- `vs2022` builds compile against the Windows 7 API level, which hides many newer declarations. This only catches part of the problem, because some newer virtdisk enum values aren't gated in the SDK headers.
- The program only imports virtdisk functions that the SDK headers don't gate behind a newer Windows, so a missing function shouldn't stop it loading.
- VHD images use version 1 parameters. Only VHDX uses version 2, and whether VHDX is available is decided at run time.
- `fmifs.dll` is loaded by full path from the system directory, rather than with a flag that needs a Windows 7 update.
- `vs2026` builds can't run on Windows 7, because the v145 standard library requires Windows 10.

## Command line tool

See [docs/cli.md](docs/cli.md) for how to use it. The commands are `create`, `attach`, `detach`, `format`, `list`, `version` and `help`. Commands name image files, never drive letters, and `format` needs `--yes`.

For scripts:

- Results go to standard output, one per line, and everything else goes to standard error. `list` separates its fields with tabs.
- Exit codes are stable: 0 for success, 1 if the operation failed, and 2 if the command line was wrong and nothing was attempted.

## Test suite

Tests are written here and run by a person, so they have to be easy to carry to a VM and their results easy to send back.

- **Packaging:** one statically linked `VirtualDiskTests.exe` and one PowerShell runner, which the build puts side by side. Nothing needs installing on the VM.
- **Unit tests:** use the fake backend, so they need no admin rights or real disks. They cover lifecycle, moves, safety refusals, every error path and argument parsing.
- **Windows tests:** use the real backend but change no disks and need no admin rights. They create images and check that no real disk is reported as virtual.
- **Integration tests:** use real disks and need admin rights. They run on the Windows 10 VM, across image format, allocation, file system and partition style. They cover the library, and the command line tool's commands through the same code `main` calls.
- **Report:** the runner writes one file holding the OS build, whether it was elevated, each test's result and any leftovers. That file is all that needs to come back.

GoogleTest comes from vcpkg in manifest mode, and the `builtin-baseline` commit in `vcpkg.json` pins its version. It is an optional `tests` feature, so `-DVDT_BUILD_TESTS=OFF` needs no dependencies at all.

Each preset has its own triplet in `triplets/`, which sets the static CRT and pins the toolset. The pin matters because vcpkg otherwise uses the newest compiler it can find, and a `vs2022` build can't reliably link a library built by a newer compiler than its own.

Rules for integration tests:

- A failed step fails the test. A test must never pass because an operation it needed didn't work.
- Without admin rights they are reported as skipped, and the runner treats that as a failed run, so a skip can't look like a pass. The one exception is ReFS, because many Windows editions can't create it.
- They only touch images inside their own temporary folder.
- Each test checks afterwards that it left no attached disk and no image file.
- The runner records the machine's real disks before and after, and fails if anything changed. Where Windows won't show the disks, as in Windows Sandbox, the tests still run but the best possible result is INCOMPLETE.
