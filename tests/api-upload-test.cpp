#include <chrono>
#include <iostream>
#include <thread>

#include "service/middleware/api_upload.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Action>
void rejects(Action action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "invalid upload operation was accepted");
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 3) {
        const std::filesystem::path directory(argv[2]);
        if (std::string_view(argv[1]) == "--recover") {
            std::cout << service::channel::UploadedFile::recoverAbandoned(directory) << '\n';
            return 0;
        }
        if (std::string_view(argv[1]) == "--hold") {
            service::channel::UploadedFile file("00000000-0000-4000-8000-000000000003", directory, 3);
            file.appendBytes("a");
            std::cout << "READY" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(60));
            return 0;
        }
        return 2;
    }
    const auto directory = std::filesystem::current_path() / ("api-upload-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    try {
        const auto orphan = directory / "00000000-0000-4000-8000-000000000001.upload";
        const auto activeId = "00000000-0000-4000-8000-000000000002";
        {
            service::channel::UploadedFile active(activeId, directory, 3);
            active.appendBytes("a");
            {
                std::ofstream output(orphan);
                output << "partial bytes";
            }
            require(service::channel::UploadedFile::recoverAbandoned(directory) == 1, "recovery did not remove only the abandoned upload");
            require(std::filesystem::exists(active.path), "recovery removed a live worker upload");
            active.appendBytes("bc");
            active.finish();
            require(service::channel::UploadedFile::recoverAbandoned(directory) == 0, "recovery removed a finalizing upload");
        }
        require(!std::filesystem::exists(orphan), "orphan remained after recovery");
        {
            service::channel::UploadedFile file("partial", directory, 3);
            rejects([&] {
                file.appendBytes("");
            });
            rejects([&] {
                file.appendBytes(std::string(16385, 'x'));
            });
            rejects([&] {
                file.finish();
            });
            file.appendBytes("a");
            require(file.received == 1, "persisted byte count differs");
            rejects([&] {
                file.appendBytes("abc");
            });
            file.appendBytes("bc");
            const auto digest = file.finish();
            require(digest == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "incremental digest differs");
            require(file.finish() == digest, "completed digest changed");
            rejects([&] {
                file.appendBytes("a");
            });
            require(std::filesystem::file_size(file.path) == 3, "completed file size differs");
        }
        require(!std::filesystem::exists(directory / "partial.upload"), "abandoned staged file remains");
        {
            service::channel::UploadedFile file("registered", directory, 1);
            file.appendBytes("a");
            file.finish();
            std::filesystem::rename(file.path, directory / "registered.bin");
        }
        require(std::filesystem::exists(directory / "registered.bin"), "registered firmware was deleted");
        std::filesystem::remove(directory / "registered.bin");
        std::filesystem::remove(directory / ".upload-recovery.lock");
        std::filesystem::remove(directory);
        std::cout << "HTTP upload bounds, digest and cleanup passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove(directory / "partial.upload");
        std::filesystem::remove(directory / "registered.upload");
        std::filesystem::remove(directory / "registered.bin");
        std::filesystem::remove(directory / ".upload-recovery.lock");
        std::filesystem::remove(directory);
        return 1;
    }
}
