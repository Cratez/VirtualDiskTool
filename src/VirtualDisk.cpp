#include "vdt/VirtualDisk.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace vdt {

namespace {

constexpr int kTemporaryDirectoryAttempts = 16;

std::u8string LowercaseExtension(const fs::path& image) {
	// Not string(), which throws for a name the ANSI code page can't represent.
	std::u8string extension = image.extension().u8string();
	std::transform(extension.begin(), extension.end(), extension.begin(),
		[](char8_t c) { return c >= u8'A' && c <= u8'Z' ? static_cast<char8_t>(c - u8'A' + u8'a') : c; });
	return extension;
}

std::optional<ImageFormat> FormatFromExtension(const fs::path& image) {
	const std::u8string extension = LowercaseExtension(image);
	if (extension == u8".vhd") return ImageFormat::Vhd;
	if (extension == u8".vhdx") return ImageFormat::Vhdx;
	return std::nullopt;
}

const char* ExtensionForFormat(ImageFormat format) noexcept {
	switch (format) {
	case ImageFormat::Vhd:  return ".vhd";
	case ImageFormat::Vhdx: return ".vhdx";
	default:                return ".img";
	}
}

/// <summary>
/// Turns Auto into a concrete format, and rejects a format the extension contradicts.
/// </summary>
Result<ImageFormat> ResolveFormat(const fs::path& image, ImageFormat requested, const IBackend& backend) {
	const std::optional<ImageFormat> fromExtension = FormatFromExtension(image);
	const ImageFormat format = requested != ImageFormat::Auto
		? requested
		: fromExtension.value_or(backend.GetDefaultFormat());

	if (!backend.Supports(format)) {
		return Error{ Errc::Unsupported, "Image format" };
	}
	const bool needsExtension = format == ImageFormat::Vhd || format == ImageFormat::Vhdx;
	if (needsExtension ? fromExtension != format : fromExtension.has_value()) {
		return Error{ Errc::InvalidArgument, "Image extension does not match its format" };
	}
	return format;
}

Result<void> ValidateVolume(const VolumeSpec& volume, const AttachOptions& options, const IBackend& backend) {
	if (volume.fileSystem == FileSystem::None) {
		return {};
	}
	if (!backend.Supports(volume.fileSystem)) {
		return Error{ Errc::Unsupported, "File system" };
	}
	if (options.readOnly) {
		return Error{ Errc::InvalidArgument, "A read-only disk cannot be formatted" };
	}
	return {};
}

/// <summary>
/// The only route to a destructive backend call. Anything but a clear yes from the OS stops here.
/// </summary>
Result<void> InitializeIfVirtual(IAttachment& attachment, const VolumeSpec& volume) {
	Result<bool> isVirtual = attachment.IsFileBackedVirtualDisk();
	if (!isVirtual) {
		return Error{ Errc::NotVirtualDisk, "Safety check failed: " + isVirtual.GetError().Message() };
	}
	if (!*isVirtual) {
		return Error{ Errc::NotVirtualDisk, "Safety check" };
	}
	return attachment.Initialize(volume);
}

/// <summary>
/// Checks everything that can be checked before touching the disk, and resolves Auto.
/// </summary>
Result<DiskSpec> ResolveSpec(const fs::path& image, const DiskSpec& spec, const IBackend& backend) {
	if (spec.size == 0) {
		return Error{ Errc::InvalidArgument, "Disk size must not be zero" };
	}
	Result<ImageFormat> format = ResolveFormat(image, spec.format, backend);
	if (!format) {
		return format.GetError();
	}
	std::error_code existsError;
	if (fs::exists(image, existsError) || existsError) {
		return Error{ existsError ? existsError : make_error_code(Errc::AlreadyExists), "Create image" };
	}

	DiskSpec resolved = spec;
	resolved.format = *format;
	return resolved;
}

Result<fs::path> CreateTemporaryDirectory() {
	std::error_code error;
	const fs::path root = fs::temp_directory_path(error);
	if (error) {
		return Error{ error, "Find temporary directory" };
	}

	std::random_device random;
	for (int attempt = 0; attempt < kTemporaryDirectoryAttempts; ++attempt) {
		char name[32];
		std::snprintf(name, sizeof(name), "vdt-%08x%08x", random(), random());
		const fs::path directory = root / name;
		if (fs::create_directory(directory, error)) {
			return directory;
		}
		if (error) {
			return Error{ error, "Create temporary directory" };
		}
	}
	return Error{ std::make_error_code(std::errc::file_exists), "Create temporary directory" };
}

/// <summary>
/// Removes an image this library created. Only ever removes the directory if it is a
/// temporary one and is empty, so it cannot take anything else with it.
/// </summary>
std::error_code RemoveImage(const fs::path& image, bool removeDirectory) {
	std::error_code error;
	fs::remove(image, error);
	if (!error && removeDirectory) {
		fs::remove(image.parent_path(), error);
	}
	return error;
}

} // anonymous namespace

VirtualDisk::VirtualDisk(
	IBackend& backend,
	std::unique_ptr<IAttachment> attachment,
	fs::path image,
	fs::path mountPath,
	const AttachOptions& options,
	bool deleteImage)
	: m_backend(&backend)
	, m_attachment(std::move(attachment))
	, m_imagePath(std::move(image))
	, m_mountPath(std::move(mountPath))
	, m_options(options)
	, m_deleteImage(deleteImage) {
}

Result<VirtualDisk> VirtualDisk::CreateAt(
	const fs::path& image,
	const DiskSpec& spec,
	const AttachOptions& options,
	IBackend& backend,
	bool deleteImage) {

	if (Result<void> valid = ValidateVolume(spec.volume, options, backend); !valid) {
		return valid.GetError();
	}
	Result<DiskSpec> resolved = ResolveSpec(image, spec, backend);
	if (!resolved) {
		return resolved.GetError();
	}
	if (Result<void> created = backend.CreateImage(image, *resolved); !created) {
		return created.GetError();
	}

	Result<std::unique_ptr<IAttachment>> attachment = backend.Attach(image, options);
	if (!attachment) {
		RemoveImage(image, false);
		return attachment.GetError();
	}

	fs::path mountPath;
	if (spec.volume.fileSystem != FileSystem::None) {
		Result<void> initialized = InitializeIfVirtual(**attachment, spec.volume);
		if (initialized) {
			Result<fs::path> mounted = (*attachment)->Mount(options.mountPath);
			if (mounted) {
				mountPath = *mounted;
			}
			else {
				initialized = mounted.GetError();
			}
		}
		if (!initialized) {
			(void)(*attachment)->Detach();
			attachment->reset();
			RemoveImage(image, false);
			return initialized.GetError();
		}
	}

	return VirtualDisk(backend, std::move(*attachment), image, std::move(mountPath), options, deleteImage);
}

Result<VirtualDisk> VirtualDisk::CreateTemporary(const DiskSpec& spec, const AttachOptions& options, IBackend& backend) {
	if (options.lifetime != Lifetime::Scoped) {
		return Error{ Errc::InvalidArgument, "A temporary disk must use Lifetime::Scoped" };
	}
	const ImageFormat format = spec.format == ImageFormat::Auto ? backend.GetDefaultFormat() : spec.format;

	Result<fs::path> directory = CreateTemporaryDirectory();
	if (!directory) {
		return directory.GetError();
	}
	const fs::path image = *directory / (std::string("disk") + ExtensionForFormat(format));

	Result<VirtualDisk> disk = CreateAt(image, spec, options, backend, true);
	if (!disk) {
		std::error_code ignored;
		fs::remove(*directory, ignored);
	}
	return disk;
}

Result<VirtualDisk> VirtualDisk::Create(const fs::path& image, const DiskSpec& spec, const AttachOptions& options, IBackend& backend) {
	return CreateAt(image, spec, options, backend, false);
}

Result<VirtualDisk> VirtualDisk::Open(const fs::path& image, const AttachOptions& options, IBackend& backend) {
	std::error_code error;
	if (!fs::is_regular_file(image, error)) {
		return Error{ error ? error : std::make_error_code(std::errc::no_such_file_or_directory), "Open image" };
	}

	// Checked here rather than left to the OS, because whether Windows refuses a second attach
	// can depend on whether the disk has been written to.
	Result<std::vector<AttachedImage>> attached = backend.ListAttached();
	if (!attached) {
		return attached.GetError();
	}
	for (const AttachedImage& entry : *attached) {
		std::error_code sameFileError;
		if (fs::equivalent(entry.image, image, sameFileError) && !sameFileError) {
			return Error{ Errc::AlreadyAttached, "Open image" };
		}
	}

	Result<std::unique_ptr<IAttachment>> attachment = backend.Attach(image, options);
	if (!attachment) {
		return attachment.GetError();
	}

	fs::path mountPath;
	Result<fs::path> mounted = (*attachment)->Mount(options.mountPath);
	if (mounted) {
		mountPath = *mounted;
	}
	else if (options.mountPath || mounted.GetError().code != Errc::NoVolume) {
		// A blank disk is fine unless the caller asked for it to be mounted somewhere.
		(void)(*attachment)->Detach();
		return mounted.GetError();
	}

	return VirtualDisk(backend, std::move(*attachment), image, std::move(mountPath), options, false);
}

VirtualDisk::~VirtualDisk() {
	Close();
}

VirtualDisk::VirtualDisk(VirtualDisk&& other) noexcept
	: m_backend(other.m_backend)
	, m_attachment(std::move(other.m_attachment))
	, m_imagePath(std::move(other.m_imagePath))
	, m_mountPath(std::move(other.m_mountPath))
	, m_options(std::move(other.m_options))
	, m_deleteImage(std::exchange(other.m_deleteImage, false)) {
	other.m_mountPath.clear();
}

VirtualDisk& VirtualDisk::operator=(VirtualDisk&& other) noexcept {
	if (this != &other) {
		Close();
		m_backend = other.m_backend;
		m_attachment = std::move(other.m_attachment);
		m_imagePath = std::move(other.m_imagePath);
		m_mountPath = std::move(other.m_mountPath);
		m_options = std::move(other.m_options);
		m_deleteImage = std::exchange(other.m_deleteImage, false);
		other.m_mountPath.clear();
	}
	return *this;
}

void VirtualDisk::Close() noexcept {
	if (!m_attachment) {
		return;
	}
	try {
		if (m_options.lifetime == Lifetime::Persistent) {
			m_attachment->Release();
		}
		else {
			(void)m_attachment->Detach();
		}
		m_attachment.reset();
		(void)DeleteImageIfTemporary();
	}
	catch (...) {
		// Only reachable by running out of memory while building an Error. A destructor has
		// no way to report that, and a Scoped attachment still ends when its handle closes.
	}
	m_mountPath.clear();
}

Result<void> VirtualDisk::DeleteImageIfTemporary() {
	if (!m_deleteImage) {
		return {};
	}
	m_deleteImage = false;
	if (const std::error_code error = RemoveImage(m_imagePath, true)) {
		return Error{ error, "Delete temporary image" };
	}
	return {};
}

const fs::path& VirtualDisk::GetImagePath() const noexcept {
	return m_imagePath;
}

const fs::path& VirtualDisk::GetMountPath() const noexcept {
	return m_mountPath;
}

Result<void> VirtualDisk::Format(const VolumeSpec& volume) {
	if (!m_attachment) {
		return Error{ Errc::NotAttached, "Format" };
	}
	if (m_options.readOnly) {
		return Error{ Errc::InvalidArgument, "A read-only disk cannot be formatted" };
	}
	if (Result<void> valid = ValidateVolume(volume, m_options, *m_backend); !valid) {
		return valid;
	}

	if (Result<void> unmounted = m_attachment->Unmount(); !unmounted) {
		return unmounted;
	}
	m_mountPath.clear();

	if (Result<void> initialized = InitializeIfVirtual(*m_attachment, volume); !initialized) {
		return initialized;
	}
	if (volume.fileSystem == FileSystem::None) {
		return {};
	}

	Result<fs::path> mounted = m_attachment->Mount(m_options.mountPath);
	if (!mounted) {
		return mounted.GetError();
	}
	m_mountPath = *mounted;
	return {};
}

Result<void> VirtualDisk::Detach() {
	if (!m_attachment) {
		return Error{ Errc::NotAttached, "Detach" };
	}
	if (Result<void> detached = m_attachment->Detach(); !detached) {
		return detached;
	}
	m_attachment.reset();
	m_mountPath.clear();
	return DeleteImageIfTemporary();
}

Result<void> VirtualDisk::Release() {
	if (!m_attachment) {
		return Error{ Errc::NotAttached, "Release" };
	}
	if (m_options.lifetime != Lifetime::Persistent) {
		return Error{ Errc::InvalidArgument, "Only a Persistent attachment can be released" };
	}
	m_attachment->Release();
	m_attachment.reset();
	m_mountPath.clear();
	return {};
}

Result<void> CreateBlankImage(const fs::path& image, const DiskSpec& spec, IBackend& backend) {
	Result<DiskSpec> resolved = ResolveSpec(image, spec, backend);
	if (!resolved) {
		return resolved.GetError();
	}
	return backend.CreateImage(image, *resolved);
}

Result<void> DetachImage(const fs::path& image, IBackend& backend) {
	return backend.DetachImage(image);
}

Result<std::vector<AttachedImage>> ListAttached(IBackend& backend) {
	return backend.ListAttached();
}

} // namespace vdt
