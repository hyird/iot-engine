#pragma once

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <openssl/evp.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "service/common/uuid.h"

namespace service::channel {

// A request-owned staged file. Business registration moves the completed
// file to its final name; every other exit removes the staged bytes.
class UploadedFile final {
    class StagedFileLock final {
      public:
        StagedFileLock(const std::filesystem::path& path, bool create, bool directoryMutex = false) : path_(path), created_(create && !directoryMutex) {
#ifdef _WIN32
            handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, directoryMutex ? OPEN_ALWAYS : create ? CREATE_NEW
                                                                                                                                                                                     : OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT,
                                  nullptr);
            if (handle_ == INVALID_HANDLE_VALUE) {
                const auto error = GetLastError();
                if (!create && (error == ERROR_FILE_NOT_FOUND || error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED)) {
                    return;
                }
                throw std::system_error(error, std::system_category(), "open staged upload");
            }
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(handle_, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
                CloseHandle(handle_);
                handle_ = INVALID_HANDLE_VALUE;
                return;
            }
            // Reserve a byte beyond all supported upload sizes; regular writes use another handle.
            overlap_.Offset = 0xfffffffe;
            overlap_.OffsetHigh = 0xffffffff;
            locked_ = LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap_) != 0;
#else
            descriptor_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | (directoryMutex ? O_CREAT : create ? O_CREAT | O_EXCL
                                                                                                                    : 0),
                                 0600);
            if (descriptor_ < 0) {
                if (!create && (errno == ENOENT || errno == ELOOP)) {
                    return;
                }
                throw std::system_error(errno, std::generic_category(), "open staged upload");
            }
            struct stat status{};
            locked_ = ::fstat(descriptor_, &status) == 0 && S_ISREG(status.st_mode) && ::flock(descriptor_, LOCK_EX | LOCK_NB) == 0;
#endif
            if (create && !directoryMutex && !locked_) {
                release();
                std::error_code ignored;
                std::filesystem::remove(path_, ignored);
                throw std::runtime_error("Cannot lock staged upload");
            }
        }

        ~StagedFileLock() {
            if (created_ && locked_) {
                std::error_code ignored;
                std::filesystem::remove(path_, ignored);
            }
            release();
        }

        bool locked() const { return locked_; }

        StagedFileLock(const StagedFileLock&) = delete;
        StagedFileLock& operator=(const StagedFileLock&) = delete;

      private:
        void release() noexcept {
#ifdef _WIN32
            if (handle_ != INVALID_HANDLE_VALUE) {
                CloseHandle(handle_);
                handle_ = INVALID_HANDLE_VALUE;
            }
#else
            if (descriptor_ >= 0) {
                ::close(descriptor_);
                descriptor_ = -1;
            }
#endif
            locked_ = false;
        }

        std::filesystem::path path_;
        bool created_{}, locked_{};
#ifdef _WIN32
        HANDLE handle_{ INVALID_HANDLE_VALUE };
        OVERLAPPED overlap_{};
#else
        int descriptor_{ -1 };
#endif
    };

  public:
    static constexpr std::size_t kChunkBytes = 16U * 1024U;
    static constexpr std::uint64_t kMaxBytes = 128ULL * 1024ULL * 1024ULL;

    // Every Service Worker runs the same recovery. Kernel locks exclude live
    // uploads, including another worker/process, and vanish on abnormal exit.
    static std::size_t recoverAbandoned(const std::filesystem::path& directory) {
        std::error_code error;
        if (!std::filesystem::exists(directory, error)) {
            if (error) {
                throw std::filesystem::filesystem_error("inspect upload directory", directory, error);
            }
            return 0;
        }
        StagedFileLock directoryClaim(directory / ".upload-recovery.lock", true, true);
        if (!directoryClaim.locked()) {
            return 0;
        }
        std::size_t removed{};
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            const auto path = entry.path();
            if (path.extension() != ".upload" || !service::common::isUuid(path.stem().string())) {
                continue;
            }
            const auto status = entry.symlink_status(error);
            if (error == std::errc::no_such_file_or_directory) {
                error.clear();
                continue;
            }
            if (error) {
                throw std::filesystem::filesystem_error("inspect staged upload", path, error);
            }
            if (!std::filesystem::is_regular_file(status)) {
                continue;
            }
            StagedFileLock claim(path, false);
            if (!claim.locked()) {
                continue;
            }
            if (std::filesystem::remove(path, error)) {
                ++removed;
            } else if (error && error != std::errc::no_such_file_or_directory) {
                throw std::filesystem::filesystem_error("remove abandoned upload", path, error);
            }
        }
        return removed;
    }

    UploadedFile(std::string id, std::filesystem::path directory, std::uint64_t size)
        : id(std::move(id)), path(std::move(directory) / (this->id + ".upload")), size(size), digest_(EVP_MD_CTX_new(), &EVP_MD_CTX_free) {
        if (size == 0 || size > kMaxBytes) {
            throw std::invalid_argument("Invalid upload size");
        }
        if (!digest_ || EVP_DigestInit_ex(digest_.get(), EVP_sha256(), nullptr) != 1) {
            throw std::runtime_error("Cannot initialize upload digest");
        }
        recoverAbandoned(path.parent_path());
        StagedFileLock directoryClaim(path.parent_path() / ".upload-recovery.lock", true, true);
        if (!directoryClaim.locked()) {
            throw std::runtime_error("Upload storage is being recovered; retry the upload");
        }
        fileLock_ = std::make_unique<StagedFileLock>(path, true);
        output_.open(path, std::ios::binary | std::ios::trunc);
        if (!output_) {
            throw std::runtime_error("Cannot create upload file");
        }
    }

    ~UploadedFile() {
        output_.close();
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    UploadedFile(const UploadedFile&) = delete;
    UploadedFile& operator=(const UploadedFile&) = delete;

    void appendBytes(std::string_view bytes) {
        if (finished_ || bytes.empty() || bytes.size() > kChunkBytes || bytes.size() > size - received) {
            throw std::invalid_argument("Upload exceeds declared size");
        }
        output_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output_ || EVP_DigestUpdate(digest_.get(), bytes.data(), bytes.size()) != 1) {
            throw std::runtime_error("Cannot persist upload chunk");
        }
        received += bytes.size();
    }

    std::string finish() {
        if (received != size) {
            throw std::invalid_argument("Upload is incomplete");
        }
        if (finished_) {
            return hash_;
        }
        output_.flush();
        if (!output_) {
            throw std::runtime_error("Cannot flush upload file");
        }
        output_.close();
        std::array<unsigned char, 32> hash{};
        unsigned length{};
        if (EVP_DigestFinal_ex(digest_.get(), hash.data(), &length) != 1 || length != hash.size()) {
            throw std::runtime_error("Cannot finish upload digest");
        }
        finished_ = true;
        constexpr char digits[] = "0123456789abcdef";
        std::string result;
        result.reserve(64);
        for (auto value : hash) {
            result += digits[value >> 4];
            result += digits[value & 15];
        }
        hash_ = result;
        return hash_;
    }

    const std::string id;
    const std::filesystem::path path;
    const std::uint64_t size;
    std::uint64_t received{};

  private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest_;
    std::unique_ptr<StagedFileLock> fileLock_;
    std::ofstream output_;
    bool finished_{};
    std::string hash_;
};

} // namespace service::channel
