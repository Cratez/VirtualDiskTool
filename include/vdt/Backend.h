#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vdt/Result.h"
#include "vdt/Types.h"

namespace vdt {

/// <summary>
/// One attached disk, as seen by a backend. Applications use VirtualDisk instead.
/// </summary>
class IAttachment {
public:
	/// <summary>
	/// Ends the attachment unless Detach or Release already did. Must not throw.
	/// </summary>
	virtual ~IAttachment() = default;

	/// <summary>
	/// True only if the OS itself reports this device as backed by the image file.
	/// VirtualDisk calls this immediately before every destructive operation.
	/// </summary>
	virtual Result<bool> IsFileBackedVirtualDisk() = 0;

	/// <summary>
	/// Destroys the disk's contents and writes one partition and file system.
	/// The only destructive call in a backend. eraseOutsideVolume also zeroes every part of the
	/// disk the new volume won't cover, because encrypting the volume can't reach those.
	/// </summary>
	virtual Result<void> Initialize(const VolumeSpec& volume, bool eraseOutsideVolume) = 0;

	/// <summary>
	/// Mounts the disk's volume and returns where it can be reached with ordinary file APIs.
	/// Fails with Errc::NoVolume if the disk has nothing to mount.
	/// </summary>
	virtual Result<std::filesystem::path> Mount(const std::optional<std::filesystem::path>& mountPath) = 0;

	/// <summary>
	/// Removes the mount made by Mount. Succeeds if nothing is mounted.
	/// </summary>
	virtual Result<void> Unmount() = 0;

	/// <summary>
	/// Encrypts the volume Initialize just wrote, and returns a new recovery key for it.
	/// VirtualDisk calls IsFileBackedVirtualDisk immediately before this. wholeVolume means
	/// free space may hold old data, so it has to be encrypted too. Otherwise only data in
	/// use is, which is much faster and keeps a dynamic image small.
	/// </summary>
	virtual Result<std::string> Encrypt(const std::string& password, bool wholeVolume) = 0;

	/// <summary>
	/// True if the volume is encrypted and locked.
	/// </summary>
	virtual Result<bool> IsLocked() = 0;

	/// <summary>
	/// Unlocks the volume. Fails with Errc::WrongPassword if the OS rejects the secret.
	/// </summary>
	virtual Result<void> UnlockWithPassword(const std::string& password) = 0;
	virtual Result<void> UnlockWithRecoveryKey(const std::string& recoveryKey) = 0;

	/// <summary>
	/// Unmounts and ends the attachment. On failure the disk stays attached and this can be
	/// called again.
	/// </summary>
	virtual Result<void> Detach() = 0;

	/// <summary>
	/// Gives up ownership, leaving the disk attached.
	/// </summary>
	virtual void Release() noexcept = 0;
};

/// <summary>
/// The contract every platform implements. All OS-specific code lives behind this.
/// </summary>
class IBackend {
public:
	virtual ~IBackend() = default;

	virtual const char* GetName() const noexcept = 0;

	/// <summary>
	/// The format ImageFormat::Auto resolves to when the image's extension doesn't decide it.
	/// </summary>
	virtual ImageFormat GetDefaultFormat() const noexcept = 0;

	virtual bool Supports(ImageFormat format) const noexcept = 0;
	virtual bool Supports(FileSystem fileSystem) const noexcept = 0;

	/// <summary>
	/// True if this OS has an encryption mechanism at all. Encrypting can still fail with
	/// Errc::Unsupported, because that can depend on the OS edition.
	/// </summary>
	virtual bool SupportsEncryption() const noexcept = 0;

	/// <summary>
	/// True if the OS ends a Scoped attachment by itself when the process dies.
	/// </summary>
	virtual bool CleansUpAfterCrash() const noexcept = 0;

	/// <summary>
	/// Creates a blank image file. spec.format is never Auto here.
	/// Fails with Errc::AlreadyExists rather than overwriting.
	/// </summary>
	virtual Result<void> CreateImage(const std::filesystem::path& image, const DiskSpec& spec) = 0;

	virtual Result<std::unique_ptr<IAttachment>> Attach(const std::filesystem::path& image, const AttachOptions& options) = 0;

	/// <summary>
	/// Detaches an image by path, for attachments no object owns any more.
	/// Fails with Errc::NotAttached if the OS does not have that image attached.
	/// </summary>
	virtual Result<void> DetachImage(const std::filesystem::path& image) = 0;

	virtual Result<std::vector<AttachedImage>> ListAttached() = 0;
};

/// <summary>
/// The backend for the current platform. Lives for the whole process.
/// </summary>
IBackend& DefaultBackend();

} // namespace vdt
