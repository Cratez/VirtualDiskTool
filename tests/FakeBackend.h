#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "vdt/Backend.h"

namespace vdt::test {

/// <summary>
/// A directory that is removed with everything in it when the test ends.
/// </summary>
class TempDirectory {
public:
	TempDirectory() {
		std::random_device random;
		m_path = std::filesystem::temp_directory_path() / ("vdt-test-" + std::to_string(random()) + std::to_string(random()));
		std::filesystem::create_directories(m_path);
	}

	~TempDirectory() {
		std::error_code ignored;
		std::filesystem::remove_all(m_path, ignored);
	}

	TempDirectory(const TempDirectory&) = delete;
	TempDirectory& operator=(const TempDirectory&) = delete;

	const std::filesystem::path& Path() const noexcept { return m_path; }

private:
	std::filesystem::path m_path;
};

/// <summary>
/// A backend that touches no disks. It records every call, and fails any call named in
/// failures, which is how tests reach the error paths a real OS rarely produces.
/// </summary>
class FakeBackend final : public IBackend {
public:
	std::vector<std::string> calls;
	std::map<std::string, Error, std::less<>> failures;
	std::vector<ImageFormat> unsupportedFormats;
	std::vector<FileSystem> unsupportedFileSystems;
	bool reportsVirtual = true;
	int virtualChecksBeforeRefusing = -1; // After this many yes answers, say no. Negative never does.
	bool supportsEncryption = true;
	bool diskIsLocked = false;
	std::string correctPassword = "password";
	std::string correctRecoveryKey = "recovery-key";
	int attached = 0; // Attachments currently holding a disk.
	int leaked = 0;   // Attachments destroyed while still holding one.

	std::optional<std::string> lastEncryptionPassword;
	bool lastEncryptionWholeVolume = false;
	bool lastEraseOutsideVolume = false;
	std::optional<DiskSpec> lastSpec;
	std::optional<AttachOptions> lastOptions;
	std::optional<VolumeSpec> lastVolume;
	std::vector<AttachedImage> attachedImages;

	Result<void> Step(const char* name) {
		calls.emplace_back(name);
		const auto failure = failures.find(name);
		if (failure != failures.end()) {
			return failure->second;
		}
		return {};
	}

	const char* GetName() const noexcept override { return "Fake"; }
	ImageFormat GetDefaultFormat() const noexcept override { return ImageFormat::Vhdx; }

	bool Supports(ImageFormat format) const noexcept override {
		return std::find(unsupportedFormats.begin(), unsupportedFormats.end(), format) == unsupportedFormats.end();
	}

	bool Supports(FileSystem fileSystem) const noexcept override {
		return std::find(unsupportedFileSystems.begin(), unsupportedFileSystems.end(), fileSystem) == unsupportedFileSystems.end();
	}

	bool SupportsEncryption() const noexcept override { return supportsEncryption; }
	bool CleansUpAfterCrash() const noexcept override { return true; }

	Result<void> CreateImage(const std::filesystem::path& image, const DiskSpec& spec) override {
		lastSpec = spec;
		if (Result<void> step = Step("CreateImage"); !step) {
			return step;
		}
		std::ofstream(image) << "image";
		return {};
	}

	Result<std::unique_ptr<IAttachment>> Attach(const std::filesystem::path& image, const AttachOptions& options) override;

	Result<void> DetachImage(const std::filesystem::path&) override {
		return Step("DetachImage");
	}

	Result<std::vector<AttachedImage>> ListAttached() override {
		if (Result<void> step = Step("ListAttached"); !step) {
			return step.GetError();
		}
		return attachedImages;
	}
};

class FakeAttachment final : public IAttachment {
public:
	explicit FakeAttachment(FakeBackend& backend) : m_backend(backend) {
		++m_backend.attached;
	}

	~FakeAttachment() override {
		if (m_holding) {
			--m_backend.attached;
			++m_backend.leaked;
		}
	}

	Result<bool> IsFileBackedVirtualDisk() override {
		if (Result<void> step = m_backend.Step("IsFileBackedVirtualDisk"); !step) {
			return step.GetError();
		}
		if (m_backend.virtualChecksBeforeRefusing == 0) {
			return false;
		}
		if (m_backend.virtualChecksBeforeRefusing > 0) {
			--m_backend.virtualChecksBeforeRefusing;
		}
		return m_backend.reportsVirtual;
	}

	Result<void> Initialize(const VolumeSpec& volume, bool eraseOutsideVolume) override {
		m_backend.lastVolume = volume;
		m_backend.lastEraseOutsideVolume = eraseOutsideVolume;
		return m_backend.Step("Initialize");
	}

	Result<std::filesystem::path> Mount(const std::optional<std::filesystem::path>& mountPath) override {
		if (Result<void> step = m_backend.Step("Mount"); !step) {
			return step.GetError();
		}
		return mountPath.value_or("FakeMount");
	}

	Result<void> Unmount() override {
		return m_backend.Step("Unmount");
	}

	Result<std::string> Encrypt(const std::string& password, bool wholeVolume) override {
		m_backend.lastEncryptionPassword = password;
		m_backend.lastEncryptionWholeVolume = wholeVolume;
		if (Result<void> step = m_backend.Step("Encrypt"); !step) {
			return step.GetError();
		}
		return m_backend.correctRecoveryKey;
	}

	Result<bool> IsLocked() override {
		if (Result<void> step = m_backend.Step("IsLocked"); !step) {
			return step.GetError();
		}
		return m_backend.diskIsLocked;
	}

	Result<void> UnlockWithPassword(const std::string& password) override {
		if (Result<void> step = m_backend.Step("UnlockWithPassword"); !step) {
			return step;
		}
		return Unlock(password == m_backend.correctPassword);
	}

	Result<void> UnlockWithRecoveryKey(const std::string& recoveryKey) override {
		if (Result<void> step = m_backend.Step("UnlockWithRecoveryKey"); !step) {
			return step;
		}
		return Unlock(recoveryKey == m_backend.correctRecoveryKey);
	}

	Result<void> Detach() override {
		if (Result<void> step = m_backend.Step("Detach"); !step) {
			return step;
		}
		Stop();
		return {};
	}

	void Release() noexcept override {
		m_backend.calls.emplace_back("Release");
		Stop();
	}

private:
	Result<void> Unlock(bool accepted) {
		if (!accepted) {
			return Error{ Errc::WrongPassword, "Unlock" };
		}
		m_backend.diskIsLocked = false;
		return {};
	}

	void Stop() noexcept {
		if (m_holding) {
			m_holding = false;
			--m_backend.attached;
		}
	}

	FakeBackend& m_backend;
	bool m_holding = true;
};

inline Result<std::unique_ptr<IAttachment>> FakeBackend::Attach(const std::filesystem::path&, const AttachOptions& options) {
	lastOptions = options;
	if (Result<void> step = Step("Attach"); !step) {
		return step.GetError();
	}
	return std::unique_ptr<IAttachment>(std::make_unique<FakeAttachment>(*this));
}

} // namespace vdt::test
