<img src="assets/icon.svg" alt="VirtualDiskTool icon: a disk inside curly braces" width="96">

# VirtualDiskTool
C++ Tool/Library for creating and managing virtual disks

It creates VHD and VHDX images, then partitions, formats and mounts them, so you get a volume you can use straight away. Images can be password protected with BitLocker. The library cleans up after itself, and neither the library nor the tool can reach a real disk by mistake.

- **[Command line tool guide](docs/cli.md):** every command, plus how to use the tool from scripts.
- **[Library guide](docs/library.md):** how to add the library to your project, and how to use it.
- **[Design](DESIGN.md):** the architecture, rules and test plan.

## Quick look

From C++:

```cpp
auto disk = vdt::VirtualDisk::CreateTemporary({ .size = vdt::MiB(64) });
if (!disk) {
	std::cerr << disk.GetError().Message() << '\n';
	return 1;
}
std::ofstream(disk->GetMountPath() / "hello.txt") << "hi";
// Leaving scope unmounts, detaches and deletes the image.
```

From an elevated prompt:

```powershell
VirtualDiskTool create test.vhdx --size 1G --fs ntfs --label Test
VirtualDiskTool attach test.vhdx
VirtualDiskTool detach test.vhdx
```

Attaching and formatting need admin rights, because Windows requires them.

## Supported platforms

Windows 10 is the only supported platform, because it is the only one fully tested. The test suite passes on Windows 10 22H2 (build 19045).

The code avoids anything known to break Windows 7, but Windows 7 is untested and unsupported.

On Windows 11, the suite has only run in Windows Sandbox, where a full (not quick) NTFS format fails. It isn't known yet whether that affects Windows 11 generally. Quick NTFS formats, and full exFAT and FAT32 formats, work there.

ReFS is untested, because no test machine so far has been able to create it. `--fs refs` is still accepted, and it fails with an error where Windows refuses it.

Password protection needs Windows Pro, Enterprise or Education, and has other limits. Its tests pass on real disks on Windows 10. See [Encryption](docs/cli.md#encryption).

## Building

You need Visual Studio 2022 or 2026 with the C++ workload and these two components: C++ CMake tools for Windows, and vcpkg package manager.

To generate the solution and project files, run this from any PowerShell prompt:

```powershell
./generate.ps1 vs2026
```

The script finds the matching Visual Studio's CMake and vcpkg, and prints the path of the solution, which you can open and build as usual.

| Preset | Toolset | Windows API level |
|---|---|---|
| `vs2022` | v143 | Windows 7 |
| `vs2026` | v145 | Windows 10 |

To build without opening Visual Studio, run this from a Developer Command Prompt:

```sh
cmake --build --preset vs2026-release
```

Build presets add `-debug` or `-release` to the name, and output goes to `build/<preset>/<config>/`.

The first time you generate a preset, vcpkg downloads and builds GoogleTest, which takes about a minute. vcpkg keeps its own tools and a cache in `%LOCALAPPDATA%\vcpkg`, which can reach several hundred MB and is safe to delete. To build without the tests, and so without any dependencies, configure with `-DVDT_BUILD_TESTS=OFF`.

## Testing

Each build puts `VirtualDiskTests.exe` and `Run-Tests.ps1` side by side in `build/<preset>/<config>/`. Running the exe by itself is safe anywhere, and without admin rights it skips the tests that attach real disks.

For a full run, copy both files to a test machine, then run this from an elevated PowerShell prompt:

```powershell
powershell -ExecutionPolicy Bypass -File .\Run-Tests.ps1
```

It writes `VirtualDiskTests-report.txt` next to the script, holding everything needed to review the run. Use a VM if you can: the tests only touch images they create themselves, but they do attach and format virtual disks as an administrator.
