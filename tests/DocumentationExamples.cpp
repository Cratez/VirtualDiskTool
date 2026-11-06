// The examples from docs/library.md and README.md, so that a change to the API can't quietly
// leave the documentation wrong. They are compiled but never run, because they need admin
// rights and fixed paths. If one has to change here, change it in the guide too.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>

#include <vdt/VirtualDisk.h>

namespace examples {

int TemporaryDisk() {
	auto disk = vdt::VirtualDisk::CreateTemporary({ .size = vdt::MiB(64) });
	if (!disk) {
		std::cerr << disk.GetError().Message() << '\n';
		return 1;
	}

	std::ofstream(disk->GetMountPath() / "hello.txt") << "hi";
	return 0;
}

int ChosenOptions() {
	auto disk = vdt::VirtualDisk::CreateTemporary({
		.size = vdt::GiB(1),
		.format = vdt::ImageFormat::Vhdx,
		.allocation = vdt::Allocation::Fixed,
		.volume = {
			.fileSystem = vdt::FileSystem::ExFat,
			.partitionStyle = vdt::PartitionStyle::Mbr,
			.label = "Scratch",
			.quickFormat = false } });
	return disk ? 0 : 1;
}

int KeptImage() {
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
	return 0;
}

int Persistent() {
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
	return detached ? 0 : 1;
}

int Reformat(vdt::VirtualDisk& disk) {
	auto formatted = disk.Format({ .fileSystem = vdt::FileSystem::Fat32, .label = "Fresh" });
	return formatted ? 0 : 1;
}

int List() {
	auto attached = vdt::ListAttached();
	if (!attached) {
		return 1;
	}
	for (const vdt::AttachedImage& image : *attached) {
		std::wcout << image.image.wstring() << L" is " << image.device.wstring() << L'\n';
	}
	return 0;
}

int Encrypted() {
	auto disk = vdt::VirtualDisk::Create("C:\\Images\\secret.vhdx", {
		.size = vdt::GiB(1),
		.volume = { .password = "a long passphrase" } });
	if (!disk) {
		std::cerr << disk.GetError().Message() << '\n';
		return 1;
	}

	// Save this somewhere other than next to the image.
	std::ofstream("D:\\Keys\\secret.key") << disk->GetRecoveryKey();
	return 0;
}

int OpenEncrypted() {
	vdt::AttachOptions options;
	options.password = "a long passphrase";

	auto disk = vdt::VirtualDisk::Open("C:\\Images\\secret.vhdx", options);
	if (!disk) {
		if (disk.GetError().code == vdt::Errc::WrongPassword) {
			std::cerr << "Wrong password.\n";
		}
		return 1;
	}
	return 0;
}

int Errors() {
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
	return 0;
}

} // namespace examples
