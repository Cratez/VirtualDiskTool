# Command line tool

`VirtualDiskTool.exe` creates and manages VHD and VHDX images from a prompt or a script. It is a single file that only needs DLLs that ship with Windows, so you can copy it anywhere. See the [README](../README.md#building) to build it.

## Quick start

From an elevated prompt:

```powershell
VirtualDiskTool create test.vhdx --size 1G --fs ntfs --label Test
VirtualDiskTool attach test.vhdx
VirtualDiskTool list
VirtualDiskTool detach test.vhdx
```

`attach` prints where the volume is mounted, such as `Z:\`. From then on it behaves like any other drive, until you run `detach`.

## Admin rights

Windows requires admin rights for most of this, so run the tool from an elevated prompt.

| Command | Needs admin rights |
|---|---|
| `create` | Yes, unless you pass `--fs none` without `--attach` |
| `attach`, `detach`, `format` | Yes |
| `list` | No, but without them the device and mount path columns are empty |
| `version`, `help` | No |

## Commands

### create

```
VirtualDiskTool create <image> --size <size> [options]
```

Creates an image holding one formatted volume. By default it attaches the image to do that, then detaches it again, leaving a ready-made file.

| Option | Meaning | Default |
|---|---|---|
| `--size <size>` | Required. See [Sizes](#sizes). | |
| `--format <format>` | `vhd` or `vhdx`. It must match the file extension. | From the extension |
| `--fixed` | Allocate the full size up front. | Grows as needed |
| `--fs <name>` | See [File systems](#file-systems). | `ntfs` |
| `--partition <style>` | `gpt` or `mbr`. | `gpt` |
| `--label <text>` | Volume label. | None |
| `--full-format` | Do a full format instead of a quick one. | Quick |
| `--attach` | Leave the disk attached, and print where it is mounted. | Detach |
| `--mount <path>` | With `--attach`: mount here. See [Mounting](#mounting). | Next free drive letter |

The image path must end in `.vhd` or `.vhdx`. Without `--attach` the command prints the image path.

`create` never overwrites an existing file. If any step fails, it leaves no image behind.

### attach

```
VirtualDiskTool attach <image> [--read-only] [--mount <path>]
```

Attaches an image and prints where its volume is mounted. The disk stays attached after the tool exits, until you run `detach`.

A blank image has nothing to mount, so `attach` prints an empty line. Combining that with `--mount` is an error.

### detach

```
VirtualDiskTool detach <image>
```

Unmounts and detaches an image. It fails if the image isn't attached.

### format

```
VirtualDiskTool format <image> --yes [--fs <name>] [--partition <style>] [--label <text>] [--full-format]
```

Erases a detached image and writes one new volume, leaving the image detached. `--yes` confirms the erase, and the options mean the same as for `create`.

It refuses an image that is attached, so detach it first.

### list

```
VirtualDiskTool list
```

Prints one line per attached virtual disk, including ones this tool didn't attach. The fields are separated by tabs: image, device, mount path.

### version and help

`version` prints the version, the Windows API level and the backend of this build. `help` prints a summary of this page.

## Sizes

A size is a whole number with an optional suffix, in either case:

| Suffix | Unit |
|---|---|
| None | Bytes |
| `K` | KiB (1024 bytes) |
| `M` | MiB |
| `G` | GiB |
| `T` | TiB |

Windows rejects images under 3 MiB, with the error `The parameter is incorrect`. A volume can need more than that: FAT32 fails at 8 MiB, for example. The tests use 64 MiB and up.

## File systems

| `--fs` | Notes |
|---|---|
| `ntfs` | The default. |
| `default` | The platform's native file system, so `ntfs` on Windows. |
| `exfat`, `fat32`, `fat`, `udf` | Tested. |
| `refs` | Untested. Many Windows editions refuse it, which is reported as an error. |
| `none` | No partition table and nothing mounted. |
| `ext4`, `xfs`, `btrfs`, `apfs`, `hfsplus` | Reserved for other platforms. On Windows they fail as not supported. |

Names aren't case sensitive. An unknown name is an error, and the tool never falls back to a different file system.

## Mounting

`--mount` takes a drive letter such as `Z:\`, or an empty folder on an NTFS volume. Without it, the tool uses the highest free drive letter, working down from `Z:`.

## Scripts

The tool is built to be scripted:

- A result goes to standard output, one per line. Errors and usage messages go to standard error.
- Exit codes don't change between versions:

| Exit code | Meaning |
|---|---|
| 0 | Success. |
| 1 | The operation was attempted and failed. |
| 2 | The command line was wrong, so nothing was attempted. |

So capturing a mount path looks like this:

```powershell
$mount = VirtualDiskTool attach test.vhdx
if ($LASTEXITCODE -ne 0) { throw 'attach failed' }

Copy-Item .\payload.bin $mount

VirtualDiskTool detach test.vhdx
```

And reading `list` looks like this:

```powershell
VirtualDiskTool list | ForEach-Object {
	$image, $device, $mount = $_ -split "`t"
	"$image is $device, mounted at $mount"
}
```

Output is always UTF-8. If non-ASCII paths come out garbled in a script, set `[Console]::OutputEncoding = [System.Text.Encoding]::UTF8` first.

## Safety

- Commands take image paths, never drive letters or device names, so a typo can't reach a real drive.
- Before writing to a disk, the tool has Windows confirm that it is a file-backed virtual disk.
- `format` needs `--yes`, and refuses an image that is attached.
- `create` never overwrites a file.

## Known limits

- A disk holds exactly one volume.
- Windows 10 is the only supported version. See [Supported platforms](../README.md#supported-platforms).
- A full NTFS format (`--full-format`) failed in Windows Sandbox on Windows 11. Quick formats work there.
- ReFS is untested.
