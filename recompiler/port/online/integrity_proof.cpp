#include "integrity_proof.h"
#include "hashing.h"

#include <fstream>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace rbrew_native {
namespace {

fs::path utf8_path(const std::string& text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

// sha256 of a file with CR bytes removed (autocrlf checkouts and ZIPs hash the same); nullopt when unreadable.
std::optional<std::string> hash_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    Sha256 hash;
    std::vector<char> block(1 << 20);
    std::string kept;
    while (in) {
        in.read(block.data(), std::streamsize(block.size()));
        const auto got = size_t(in.gcount());
        kept.clear();
        for (size_t i = 0; i < got; ++i)
            if (block[i] != '\r') kept += block[i];
        hash.update(kept.data(), kept.size());
    }
    if (in.bad()) return std::nullopt;
    return hash.hex();
}

// [(sha256, relative path)] of the manifest, or nullopt when the install has none.
std::optional<std::vector<std::pair<std::string, std::string>>> read_manifest(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::pair<std::string, std::string>> entries;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line;
        for (size_t i = start; i < end; ++i)
            if (text[i] != '\r') line += text[i];
        const size_t space = line.find(' ');
        const std::string digest = line.substr(0, space);
        const std::string name = space == std::string::npos ? std::string() : line.substr(space + 1);
        if (digest.size() == 64 && !name.empty()) entries.emplace_back(digest, name);
        start = end + 1;
    }
    return entries;
}

} // namespace

std::string read_version(const fs::path& root) {
    std::ifstream in(root / "VERSION", std::ios::binary);
    if (!in) return {};
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
    };
    while (!text.empty() && is_space(text.back())) text.pop_back();
    size_t first = 0;
    while (first < text.size() && is_space(text[first])) ++first;
    while (first < text.size() && text[first] == 'v') ++first;
    text = text.substr(first);
    if (text.empty()) return {};
    for (char c : text)
        if (!(c >= '0' && c <= '9') && c != '.') return {};
    return text;
}

Attestation attest(const fs::path& root) {
    Attestation result{read_version(root), {}};
    const auto entries = read_manifest(root / "scripts" / "integrity-manifest.txt");
    if (!entries || entries->empty()) return result;
    std::string text;
    for (const auto& [digest, name] : *entries) {
        const auto mine = hash_file(root / utf8_path(name));
        text += mine ? *mine : std::string(64, '0'); // a missing file changes the result
        text += ' ';
        text += name;
        text += '\n';
    }
    result.code = sha256_hex(text);
    return result;
}

} // namespace rbrew_native
