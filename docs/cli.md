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
| `--encrypt` | Protect the volume with a password. See [Encryption](#encryption). | Not encrypted |
| `--recovery-key-file <path>` | With `--encrypt`, and required: save the recovery key here. | |

The image path must end in `.vhd` or `.vhdx`. Without `--attach` the command prints the image path.

`create` never overwrites an existing file. If any step fails, it leaves no image behind.

### attach

```
VirtualDiskTool attach <image> [--read-only] [--mount <path>] [--unlock | --recovery-key-file <path>]
```

Attaches an image and prints where its volume is mounted. The disk stays attached after the tool exits, until you run `detach`.

A blank image has nothing to mount, so `attach` prints an empty line. Combining that with `--mount` is an error.

An encrypted image needs `--unlock`, which asks for the password, or `--recovery-key-file`, which reads a saved key. Without either, `attach` fails and leaves the image detached. Using either on an image that isn't encrypted is also an error.

### detach

```
VirtualDiskTool detach <image>
```

Unmounts and detaches an image. It fails if the image isn't attached.

### format

```
VirtualDiskTool format <image> --yes [--fs <name>] [--partition <style>] [--label <text>] [--full-format] [--encrypt --recovery-key-file <path>]
```

Erases a detached image and writes one new volume, leaving the image detached. `--yes` confirms the erase, and the options mean the same as for `create`.

It refuses an image that is attached, so detach it first. It works on an encrypted image without its password, and the result isn't encrypted unless you pass `--encrypt` again.

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

## Encryption

`--encrypt` protects a volume with a password, using BitLocker. A copy of the image file is unreadable without the password or recovery key.

```powershell
VirtualDiskTool create secret.vhdx --size 1G --encrypt --recovery-key-file D:\Keys\secret.key
VirtualDiskTool attach secret.vhdx --unlock
VirtualDiskTool detach secret.vhdx
```

The tool asks for the password without showing it, and asks twice when setting a new one. Detaching locks the disk again.

Passwords are never command line arguments, because those end up in shell history and process lists. A script can pipe one in instead, as a single line on standard input:

```powershell
$password | VirtualDiskTool attach secret.vhdx --unlock
```

If a password has non-ASCII characters, check that your shell sends it as UTF-8. In Windows PowerShell, set `$OutputEncoding = New-Object System.Text.UTF8Encoding($false)` first. Windows PowerShell can also add byte order marks to what it pipes, which the tool ignores.

### The recovery key

`--recovery-key-file` is required, because a forgotten password with no recovery key means the data is gone for good. The tool won't overwrite an existing file. To use the key:

```powershell
VirtualDiskTool attach secret.vhdx --recovery-key-file D:\Keys\secret.key
```

Treat the key file like the password, because anyone who has it can unlock the image. Don't keep it next to the image. The tool doesn't restrict who can read it.

### What it doesn't do

- **It only protects data at rest.** While a disk is attached and unlocked, anything on that machine with access to the volume can read it. Detach it when you're done.
- **It doesn't stop anyone destroying data.** `format --yes` needs no password, and anyone with access to the file can delete it.
- **It can't be added to a disk that already holds data.** Encrypting happens as part of `create` or `format`, which start with an empty volume.
- **It can't change or remove a password.** To do either, copy the data off and `format` the image.

### Limits

- **Windows edition:** BitLocker needs Windows Pro, Enterprise or Education. Home editions can't create encrypted images.
- **Disk size:** a 64 MiB disk is too small and a 256 MiB disk works. The exact minimum isn't known yet.
- **File systems:** Microsoft documents this as NTFS only. exFAT worked in testing on Windows 10, but treat it as unofficial.
- **Passwords:** BitLocker can reject a short or simple password, and Group Policy can forbid passwords altogether.
- **Image size:** encrypting costs a roughly fixed amount of space in a new dynamic VHDX, rather than a share of its size. An encrypted VHDX used 196 MiB for a 256 MiB disk, 292 to 356 MiB for 1 GiB and 388 to 420 MiB for 8 GiB, varying between runs. A VHD uses less, at 52 MiB for a 256 MiB disk. Using `format` to encrypt an existing image encrypts the whole volume, and erases the rest of the disk, because old data could still be there. So it takes longer, and a dynamic image grows to about its full size.
- **Very large disks:** `format --encrypt` gives up after 12 hours, which leaves the image partly encrypted with no key saved. Format it again to start over.
- **Older Windows:** images use Windows' default algorithm, XTS-AES 128, which Windows older than 10 version 1511 can't be expected to unlock.
- **Windows Sandbox:** encryption is untested there, and isn't expected to work, because Sandbox denied access to the WMI interfaces the tests tried, and BitLocker is managed through WMI.

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
- `create` never overwrites a file, and that includes recovery key files.

## Known limits

- A disk holds exactly one volume.
- Windows 10 is the only supported version. See [Supported platforms](../README.md#supported-platforms).
- A full NTFS format (`--full-format`) failed in Windows Sandbox on Windows 11. Quick formats work there.
- ReFS is untested.
- Encryption has [its own limits](#encryption).
