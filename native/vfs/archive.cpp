#include "archive.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace bar {
namespace {

bool read_whole_file(const std::filesystem::path& path, std::vector<uint8_t>& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "could not read " + path.string();
        return false;
    }
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size < 0) {
        error = "could not size " + path.string();
        return false;
    }
    in.seekg(0);
    out.resize(static_cast<size_t>(size));
    if (size > 0) {
        in.read(reinterpret_cast<char*>(out.data()), size);
    }
    if (!in) {
        error = "short read " + path.string();
        return false;
    }
    return true;
}

std::string seven_zip_bin() {
    const char* candidates[] = {"/opt/homebrew/bin/7z", "/usr/local/bin/7z", "7z"};
    for (const char* candidate : candidates) {
        if (candidate[0] == '/' && access(candidate, X_OK) == 0) {
            return candidate;
        }
    }
    return "7z";
}

bool read_sd7(const std::string& archive, const std::string& inner, std::vector<uint8_t>& out, std::string& error) {
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
        error = "pipe failed";
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_fd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_fd[0]);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    const std::string bin = seven_zip_bin();
    const char* argv[] = {bin.c_str(), "e", "-so", "-y", archive.c_str(), inner.c_str(), nullptr};
    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, bin.c_str(), &actions, nullptr, const_cast<char**>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_fd[1]);
    if (spawned != 0) {
        close(pipe_fd[0]);
        error = "could not run 7z (" + bin + "): " + std::strerror(spawned);
        return false;
    }

    std::vector<uint8_t> bytes;
    uint8_t buffer[1 << 16];
    while (true) {
        const ssize_t got = read(pipe_fd[0], buffer, sizeof(buffer));
        if (got == 0) {
            break;
        }
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(pipe_fd[0]);
            error = "read from 7z failed";
            return false;
        }
        bytes.insert(bytes.end(), buffer, buffer + got);
    }
    close(pipe_fd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || bytes.empty()) {
        error = "7z could not extract " + inner + " from " + archive;
        return false;
    }
    out.swap(bytes);
    return true;
}

std::string normalize_map_name(std::string name) {
    for (char& ch : name) {
        if (ch == ' ') {
            ch = '_';
        } else if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return name;
}

}  // namespace

bool Archive::read_file(const std::string& archive_path, const std::string& inner_path, std::vector<uint8_t>& out,
                        std::string& error) {
    const std::filesystem::path path(archive_path);
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        return read_whole_file(path / inner_path, out, error);
    }
    const auto ext = path.extension().string();
    if (ext == ".sd7" || ext == ".sdz" || ext == ".7z") {
        return read_sd7(archive_path, inner_path, out, error);
    }
    if (inner_path.empty() || inner_path == path.filename()) {
        return read_whole_file(path, out, error);
    }
    error = "unsupported archive " + archive_path;
    return false;
}

bool find_map_archive(const std::string& map_dir, const std::string& map_name, std::string& archive_path,
                      std::string& error) {
    const std::string normalized = normalize_map_name(map_name);
    const std::filesystem::path dir(map_dir);
    const std::filesystem::path direct = dir / (normalized + ".sd7");
    std::error_code ec;
    if (std::filesystem::is_regular_file(direct, ec)) {
        archive_path = direct.string();
        return true;
    }
    if (!std::filesystem::is_directory(dir, ec)) {
        error = "map directory does not exist: " + map_dir;
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (name.find(normalized) != std::string::npos && entry.path().extension() == ".sd7") {
            archive_path = entry.path().string();
            return true;
        }
    }
    error = "no map archive for \"" + map_name + "\" in " + map_dir;
    return false;
}

}  // namespace bar
