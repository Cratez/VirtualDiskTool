#include "vdt/Result.h"

namespace vdt {

namespace {

class Category final : public std::error_category {
public:
	const char* name() const noexcept override { return "vdt"; }

	std::string message(int value) const override {
		switch (static_cast<Errc>(value)) {
		case Errc::InvalidArgument: return "invalid argument";
		case Errc::Unsupported:     return "not supported on this platform";
		case Errc::AlreadyExists:   return "the image already exists";
		case Errc::NotAttached:     return "the disk is not attached";
		case Errc::AlreadyAttached: return "refused: the image is already attached, so detach it first";
		case Errc::NotVirtualDisk:  return "refused: the target is not a file-backed virtual disk";
		case Errc::NoVolume:        return "the disk has no volume";
		case Errc::Timeout:         return "timed out waiting for the operating system";
		case Errc::Locked:          return "the disk is encrypted, so a password or recovery key is needed";
		case Errc::WrongPassword:   return "the password or recovery key was not accepted";
		case Errc::TooSmallToEncrypt: return "the disk is too small to encrypt";
		}
		return "unknown error";
	}
};

} // anonymous namespace

const std::error_category& ErrorCategory() noexcept {
	static const Category category;
	return category;
}

} // namespace vdt
