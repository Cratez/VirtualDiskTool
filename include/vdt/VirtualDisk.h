#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "vdt/Backend.h"
#include "vdt/Result.h"
#include "vdt/Types.h"

namespace vdt {

/// <summary>
/// Owns one attached virtual disk. Move-only, and every instance holds a live attachment
/// until it is moved from, detached or released. After that only destruction, assignment
/// and GetImagePath are valid; other calls fail with Errc::NotAttached.
///
/// Instances are not thread-safe, but separate instances can be used concurrently.
/// A backend passed in must outlive the instance.
/// </summary>
class VirtualDisk {
public:
	/// <summary>
	/// Creates an image at a unique temporary path, then attaches, formats and mounts it.
	/// The image is deleted when the attachment ends. options.lifetime must be Scoped.
	/// All or nothing: on failure no image is left behind.
	/// </summary>
	static Result<VirtualDisk> CreateTemporary(
		const DiskSpec& spec,
		const AttachOptions& options = {},
		IBackend& backend = DefaultBackend());

	/// <summary>
	/// Creates an image at the given path, then attaches, formats and mounts it.
	/// Never overwrites: fails with Errc::AlreadyExists. The image outlives this object.
	/// With ImageFormat::Auto the extension decides the format, and an explicit format must
	/// agree with the extension. All or nothing: on failure no image is left behind.
	/// </summary>
	static Result<VirtualDisk> Create(
		const std::filesystem::path& image,
		const DiskSpec& spec,
		const AttachOptions& options = {},
		IBackend& backend = DefaultBackend());

	/// <summary>
	/// Attaches an existing image and mounts its volume, if it has one. Fails with
	/// Errc::AlreadyAttached if the image is attached already, so that a disk someone has
	/// mounted can't be reached and erased through a second object.
	/// </summary>
	static Result<VirtualDisk> Open(
		const std::filesystem::path& image,
		const AttachOptions& options = {},
		IBackend& backend = DefaultBackend());

	/// <summary>
	/// Ends a Scoped attachment and deletes a temporary image. Leaves a Persistent attachment
	/// in place. Failures cannot be reported from here, so call Detach to see them.
	/// </summary>
	~VirtualDisk();

	VirtualDisk(VirtualDisk&& other) noexcept;
	VirtualDisk& operator=(VirtualDisk&& other) noexcept;
	VirtualDisk(const VirtualDisk&) = delete;
	VirtualDisk& operator=(const VirtualDisk&) = delete;

	const std::filesystem::path& GetImagePath() const noexcept;

	/// <summary>
	/// Where the volume can be reached with ordinary file APIs. Empty if nothing is mounted.
	/// </summary>
	const std::filesystem::path& GetMountPath() const noexcept;

	/// <summary>
	/// Destroys the disk's contents, writes one fresh volume and mounts it again.
	/// Fails with Errc::NotVirtualDisk, changing nothing, unless the OS confirms the target
	/// is a file-backed virtual disk. There is deliberately no way to format by drive letter
	/// or device path.
	/// </summary>
	Result<void> Format(const VolumeSpec& volume);

	/// <summary>
	/// Unmounts and detaches now, reporting any failure. Works for either lifetime, and
	/// deletes a temporary image.
	/// </summary>
	Result<void> Detach();

	/// <summary>
	/// Stops managing a Persistent attachment, leaving it attached and mounted.
	/// Fails with Errc::InvalidArgument for a Scoped attachment.
	/// </summary>
	Result<void> Release();

private:
	VirtualDisk(
		IBackend& backend,
		std::unique_ptr<IAttachment> attachment,
		std::filesystem::path image,
		std::filesystem::path mountPath,
		const AttachOptions& options,
		bool deleteImage);

	static Result<VirtualDisk> CreateAt(
		const std::filesystem::path& image,
		const DiskSpec& spec,
		const AttachOptions& options,
		IBackend& backend,
		bool deleteImage);

	void Close() noexcept;
	Result<void> DeleteImageIfTemporary();

	IBackend* m_backend = nullptr;
	std::unique_ptr<IAttachment> m_attachment;
	std::filesystem::path m_imagePath;
	std::filesystem::path m_mountPath;
	AttachOptions m_options;
	bool m_deleteImage = false;
};

/// <summary>
/// Creates an image file without attaching it, so nothing is partitioned or formatted and
/// spec.volume is ignored. Unlike attaching, this usually doesn't need admin rights.
/// </summary>
Result<void> CreateBlankImage(
	const std::filesystem::path& image,
	const DiskSpec& spec,
	IBackend& backend = DefaultBackend());

/// <summary>
/// Detaches an image left attached by a Persistent attachment.
/// </summary>
Result<void> DetachImage(const std::filesystem::path& image, IBackend& backend = DefaultBackend());

/// <summary>
/// Lists every virtual disk the OS has attached, including ones this library did not attach.
/// </summary>
Result<std::vector<AttachedImage>> ListAttached(IBackend& backend = DefaultBackend());

} // namespace vdt
