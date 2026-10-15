#include "pool.h"

#include <zlib.h>

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <sstream>

namespace bar {
namespace {

std::string lower_copy(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string hex16(const uint8_t* bytes) {
    std::ostringstream out;
    out << std::hex;
    for (int i = 0; i < 16; ++i) {
        const int value = bytes[i];
        if (value < 16) {
            out << '0';
        }
        out << value;
    }
    return out.str();
}

bool gunzip_file(const std::string& path, std::vector<uint8_t>& out, std::string& error) {
    gzFile file = gzopen(path.c_str(), "rb");
    if (file == nullptr) {
        error = "could not open " + path;
        return false;
    }
    out.clear();
    uint8_t buffer[1 << 16];
    while (true) {
        const int got = gzread(file, buffer, sizeof(buffer));
        if (got == 0) {
            break;
        }
        if (got < 0) {
            gzclose(file);
            error = "gzip read failed " + path;
            return false;
        }
        out.insert(out.end(), buffer, buffer + got);
    }
    gzclose(file);
    if (out.empty()) {
        error = "empty gzip " + path;
        return false;
    }
    return true;
}

bool parse_sdp(const std::vector<uint8_t>& data, std::vector<std::pair<std::string, std::string>>& index,
               bool& has_commander) {
    index.clear();
    has_commander = false;
    std::size_t pos = 0;
    while (pos < data.size()) {
        const uint8_t name_len = data[pos];
        if (name_len == 0 || pos + 1 + name_len + 24 > data.size()) {
            break;
        }
        const std::string name(reinterpret_cast<const char*>(data.data() + pos + 1), name_len);
        const std::string md5 = hex16(data.data() + pos + 1 + name_len);
        const std::string key = lower_copy(name);
        if (key == "objects3d/units/armcom.s3o") {
            has_commander = true;
        }
        index.emplace_back(key, md5);
        pos += 1 + name_len + 24;
    }
    return !index.empty();
}

}  // namespace

bool GameContent::open(const std::string& data_dir, std::string& error) {
    const std::filesystem::path packages = std::filesystem::path(data_dir) / "packages";
    pool_dir_ = (std::filesystem::path(data_dir) / "pool").string();
    std::error_code ec;
    if (!std::filesystem::is_directory(packages, ec)) {
        error = "game packages directory does not exist: " + packages.string();
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(packages, ec)) {
        if (entry.path().extension() != ".sdp") {
            continue;
        }
        std::vector<uint8_t> bytes;
        if (!gunzip_file(entry.path().string(), bytes, error)) {
            continue;
        }
        bool has_commander = false;
        std::vector<std::pair<std::string, std::string>> index;
        if (!parse_sdp(bytes, index, has_commander) || !has_commander) {
            continue;
        }
        index_.swap(index);
        return true;
    }
    error = "no installed package contains objects3d/units/armcom.s3o";
    return false;
}

bool GameContent::read(const std::string& inner_path, std::vector<uint8_t>& out, std::string& error) const {
    const std::string key = lower_copy(inner_path);
    const std::string* md5 = nullptr;
    for (const auto& entry : index_) {
        if (entry.first == key) {
            md5 = &entry.second;
            break;
        }
    }
    if (md5 == nullptr) {
        error = "package has no " + inner_path;
        return false;
    }
    const std::string path = pool_dir_ + "/" + md5->substr(0, 2) + "/" + md5->substr(2) + ".gz";
    return gunzip_file(path, out, error);
}

}  // namespace bar
