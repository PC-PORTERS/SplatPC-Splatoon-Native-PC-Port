#include "manifest.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace rebrewu::manifest {
namespace {

// Text helpers

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// `#` and `//` both start a comment. Trailing comments are allowed after a
// value, which is how a manifest documents an entry without affecting data.
std::string strip_comment(std::string_view line) {
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '#') return std::string(line.substr(0, i));
        if (line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/') return std::string(line.substr(0, i));
    }
    return std::string(line);
}

bool all_hex(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

bool parse_hex_u32(const std::string& raw, uint32_t& out); // defined below

// Values are compared after normalization so that `0x02D09A00` and `0x02d09a00u`
// hash identically: formatting is not data.
std::string normalize_atom(const std::string& raw) {
    const std::string v = trim(raw);
    const bool prefixed = v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X');
    if (prefixed) {
        std::string t = v.substr(2);
        while (!t.empty() && (t.back() == 'u' || t.back() == 'U')) t.pop_back();
        if (all_hex(t)) return "0x" + lower(t);
        return v;
    }
    if (all_hex(v)) return lower(v);
    return v;
}

// `0xADDR mode` keeps its mode token; free-form text such as a title string
// keeps its spacing and is hashed exactly as written.
std::string normalize_value(const std::string& raw) {
    const std::string v = trim(raw);
    const auto space = v.find_first_of(" \t");
    if (space != std::string::npos) {
        const std::string head = v.substr(0, space);
        const std::string tail = trim(v.substr(space));
        uint32_t ignored = 0;
        if (parse_hex_u32(head, ignored) && !tail.empty() && tail.find_first_of(" \t") == std::string::npos)
            return normalize_atom(head) + " " + lower(tail);
    }
    return normalize_atom(v);
}

// An explicit `Undefined` hash opts a section out of verification.
bool is_undefined_hash(const std::string& raw) {
    return lower(trim(raw)) == "undefined";
}

bool parse_hex_u32(const std::string& raw, uint32_t& out) {
    std::string t = trim(raw);
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t = t.substr(2);
    while (!t.empty() && (t.back() == 'u' || t.back() == 'U')) t.pop_back();
    if (t.empty() || t.size() > 8 || !all_hex(t)) return false;
    try {
        out = static_cast<uint32_t>(std::stoul(t, nullptr, 16));
    } catch (...) {
        return false;
    }
    return true;
}

bool parse_hex_u64(const std::string& raw, uint64_t& out) {
    std::string t = trim(raw);
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t = t.substr(2);
    while (!t.empty() && (t.back() == 'u' || t.back() == 'U')) t.pop_back();
    if (t.empty() || t.size() > 16 || !all_hex(t)) return false;
    try {
        out = std::stoull(t, nullptr, 16);
    } catch (...) {
        return false;
    }
    return true;
}

std::string hex_address(uint32_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
    return buffer;
}

// SHA-256 (FIPS 180-4), used only to prove the RPX matches the manifest.

struct Sha256 {
    uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                         0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    uint8_t block[64]{};
    std::size_t used = 0;
    uint64_t total = 0;

    static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

    void compress(const uint8_t* p) {
        static constexpr uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
        uint32_t w[64];
        for (unsigned i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) |
                   uint32_t(p[i * 4 + 3]);
        for (unsigned i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (unsigned i = 0; i < 64; ++i) {
            const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = h + s1 + ch + k[i] + w[i];
            const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    void update(const uint8_t* data, std::size_t size) {
        total += size;
        while (size) {
            const std::size_t take = std::min(size, sizeof(block) - used);
            std::memcpy(block + used, data, take);
            used += take;
            data += take;
            size -= take;
            if (used == sizeof(block)) {
                compress(block);
                used = 0;
            }
        }
    }

    std::array<uint8_t, 32> finish() {
        const uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (used != 56) update(&zero, 1);
        uint8_t length[8];
        for (unsigned i = 0; i < 8; ++i) length[i] = uint8_t(bits >> (56 - i * 8));
        // update() would count these bytes; write them directly instead.
        std::memcpy(block + used, length, 8);
        compress(block);
        used = 0;
        std::array<uint8_t, 32> out{};
        for (unsigned i = 0; i < 8; ++i) {
            out[i * 4] = uint8_t(state[i] >> 24);
            out[i * 4 + 1] = uint8_t(state[i] >> 16);
            out[i * 4 + 2] = uint8_t(state[i] >> 8);
            out[i * 4 + 3] = uint8_t(state[i]);
        }
        return out;
    }
};

std::string to_hex(const std::array<uint8_t, 32>& digest) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (uint8_t byte : digest) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0xf]);
    }
    return out;
}

} // namespace

bool is_hashed_section(const std::string& name) {
    return name == "identity" || name == "modules" || name == "imports" || name == "entries" || name == "hooks" ||
           name == "patches";
}

std::string format_hash(uint64_t value) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%016llX", static_cast<unsigned long long>(value));
    return buffer;
}

std::string format_address(uint32_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
    return buffer;
}

uint64_t section_hash(const std::map<std::string, std::string>& pairs) {
    std::vector<std::pair<std::string, std::string>> ordered;
    ordered.reserve(pairs.size());
    for (const auto& [key, value] : pairs) {
        if (key == "hash") continue; // a section never hashes its own digest
        ordered.emplace_back(key, normalize_value(value));
    }
    std::sort(ordered.begin(), ordered.end());
    uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&h](unsigned char byte) {
        h ^= byte;
        h *= 0x100000001B3ull;
    };
    for (const auto& [key, value] : ordered) {
        for (char c : key) mix(static_cast<unsigned char>(c));
        mix(0x1f); // key/value separator
        for (char c : value) mix(static_cast<unsigned char>(c));
        mix('\n'); // pair terminator
    }
    return h;
}

const std::string* Manifest::identity_value(const std::string& key) const {
    const auto it = identity.find(key);
    return it == identity.end() ? nullptr : &it->second;
}

std::string Manifest::identity_string(const std::string& key) const {
    const std::string* value = identity_value(key);
    return value ? trim(*value) : std::string();
}

uint32_t Manifest::identity_u32(const std::string& key, uint32_t fallback) const {
    const std::string* value = identity_value(key);
    uint32_t parsed = 0;
    return value && parse_hex_u32(*value, parsed) ? parsed : fallback;
}

Manifest parse_text(const std::string& text, const std::string& path) {
    Manifest m;
    m.path = path;

    std::map<std::string, std::map<std::string, std::string>> sections;
    std::string current;
    std::istringstream input(text);
    std::string raw;
    unsigned line_number = 0;
    while (std::getline(input, raw)) {
        ++line_number;
        std::string line = trim(strip_comment(raw));
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() != ']') {
                m.errors.push_back("line " + std::to_string(line_number) + ": unterminated section header");
                continue;
            }
            current = lower(trim(std::string_view(line).substr(1, line.size() - 2)));
            sections[current];
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            m.errors.push_back("line " + std::to_string(line_number) + ": expected 'key = value'");
            continue;
        }
        if (current.empty()) {
            m.errors.push_back("line " + std::to_string(line_number) + ": entry outside any section");
            continue;
        }
        const std::string key = trim(std::string_view(line).substr(0, equals));
        const std::string value = trim(std::string_view(line).substr(equals + 1));
        if (key.empty()) {
            m.errors.push_back("line " + std::to_string(line_number) + ": empty key");
            continue;
        }
        auto& bucket = sections[current];
        if (!bucket.emplace(key, value).second)
            m.errors.push_back("line " + std::to_string(line_number) + ": duplicate key '" + key + "' in [" + current +
                               "]");
    }

    // Metadata and identity are plain string tables. The digest line is not
    // data, so it never becomes a module name or an identity field.
    m.meta = sections["manifest"];
    m.identity = sections["identity"];
    m.modules = sections["modules"];
    m.identity.erase("hash");
    m.modules.erase("hash");
    const auto id = m.meta.find("id");
    if (id != m.meta.end()) m.id = trim(id->second);

    const auto format = m.meta.find("format");
    if (format == m.meta.end()) {
        m.warnings.push_back("missing [manifest] format = " + std::string(kFormatVersion));
    } else if (trim(format->second) != kFormatVersion) {
        m.warnings.push_back("format '" + trim(format->second) + "' differs from '" + kFormatVersion + "'");
    }

    // [imports]: "module!symbol = 0xADDR"
    for (const auto& [key, value] : sections["imports"]) {
        if (key == "hash") continue;
        const auto bang = key.find('!');
        if (bang == std::string::npos || bang == 0 || bang + 1 >= key.size()) {
            m.errors.push_back("[imports] key '" + key + "' is not 'module!symbol'");
            continue;
        }
        Import import;
        import.module = trim(std::string_view(key).substr(0, bang));
        import.symbol = trim(std::string_view(key).substr(bang + 1));
        // Optional trailing mode: `0xADDR` (code stub, default) or `0xADDR data`.
        std::string address_text = value;
        const auto space = value.find_first_of(" \t");
        if (space != std::string::npos) {
            const std::string mode = lower(trim(value.substr(space)));
            address_text = value.substr(0, space);
            if (mode == "data")
                import.data = true;
            else {
                m.errors.push_back("[imports] " + key + ": mode must be 'data', got '" + mode + "'");
                continue;
            }
        }
        if (!parse_hex_u32(address_text, import.address)) {
            m.errors.push_back("[imports] " + key + ": bad address '" + address_text + "'");
            continue;
        }
        m.imports.push_back(std::move(import));
    }

    // [entries]: "0xADDR = normal|force|corroborate"
    {
        std::map<uint32_t, std::string> seen;
        for (const auto& [key, value] : sections["entries"]) {
            if (key == "hash") continue;
            Entry entry;
            if (!parse_hex_u32(key, entry.address)) {
                m.errors.push_back("[entries] '" + key + "' is not a 32-bit address");
                continue;
            }
            const std::string mode = lower(trim(value));
            if (mode == "force")
                entry.force = true;
            else if (mode == "corroborate")
                entry.corroborate = true;
            else if (mode != "normal")
                m.errors.push_back("[entries] " + key + ": mode must be 'normal', 'force' or 'corroborate'");
            const auto [it, inserted] = seen.emplace(entry.address, key);
            if (!inserted) {
                m.errors.push_back("[entries] " + key + " duplicates " + it->second);
                continue;
            }
            m.entries.push_back(entry);
        }
    }

    // [hooks]: "0xADDR = hook"
    {
        std::map<uint32_t, std::string> seen;
        for (const auto& [key, value] : sections["hooks"]) {
            if (key == "hash") continue;
            uint32_t address = 0;
            if (!parse_hex_u32(key, address)) {
                m.errors.push_back("[hooks] '" + key + "' is not a 32-bit address");
                continue;
            }
            if (address & 3u) {
                m.errors.push_back("[hooks] " + key + ": address is not 4-byte aligned");
                continue;
            }
            if (lower(trim(value)) != "hook") m.errors.push_back("[hooks] " + key + ": value must be 'hook'");
            const auto [it, inserted] = seen.emplace(address, key);
            if (!inserted) {
                m.errors.push_back("[hooks] " + key + " duplicates " + it->second);
                continue;
            }
            m.hooks.push_back(address);
        }
    }

    // [patches]: "0xADDR = instruction"
    {
        std::map<uint32_t, std::string> seen;
        for (const auto& [key, value] : sections["patches"]) {
            if (key == "hash") continue;
            Patch patch;
            if (!parse_hex_u32(key, patch.address)) {
                m.errors.push_back("[patches] '" + key + "' is not a 32-bit address");
                continue;
            }
            if (patch.address & 3u) {
                m.errors.push_back("[patches] " + key + ": address is not 4-byte aligned");
                continue;
            }
            patch.instruction = trim(value);
            if (patch.instruction.empty()) {
                m.errors.push_back("[patches] " + key + ": missing instruction");
                continue;
            }
            const auto [it, inserted] = seen.emplace(patch.address, key);
            if (!inserted) {
                m.errors.push_back("[patches] " + key + " duplicates " + it->second);
                continue;
            }
            m.patches.push_back(std::move(patch));
        }
    }

    std::sort(m.imports.begin(), m.imports.end(), [](const Import& a, const Import& b) {
        if (a.module != b.module) return a.module < b.module;
        return a.symbol < b.symbol;
    });
    std::sort(m.entries.begin(), m.entries.end(), [](const Entry& a, const Entry& b) { return a.address < b.address; });
    std::sort(m.hooks.begin(), m.hooks.end());
    std::sort(m.patches.begin(), m.patches.end(), [](const Patch& a, const Patch& b) { return a.address < b.address; });

    // Per-section hashes: only the section's own pairs contribute. A section
    // whose hash is `Undefined` is explicitly opted out of verification.
    for (const auto& [name, pairs] : sections) {
        if (!is_hashed_section(name)) continue;
        const auto stored = pairs.find("hash");
        if (stored != pairs.end() && is_undefined_hash(stored->second)) {
            m.ignored_hashes.insert(name);
            continue;
        }
        m.computed_hashes[name] = section_hash(pairs);
        if (stored == pairs.end()) {
            m.warnings.push_back("section [" + name + "] has no hash line");
            continue;
        }
        uint64_t value = 0;
        if (!parse_hex_u64(stored->second, value)) {
            m.errors.push_back("[" + name + "] hash is not a 64-bit value");
            continue;
        }
        m.stored_hashes[name] = value;
    }
    return m;
}

std::optional<Manifest> parse_file(const std::string& path, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "cannot read manifest " + path;
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return parse_text(buffer.str(), path);
}

bool verify_hashes(Manifest& m) {
    bool ok = m.errors.empty();
    for (const auto& [name, computed] : m.computed_hashes) {
        if (m.ignored_hashes.contains(name)) continue;
        const auto stored = m.stored_hashes.find(name);
        if (stored == m.stored_hashes.end()) {
            m.errors.push_back("[" + name + "] has no hash line (run 'manifest seal')");
            ok = false;
            continue;
        }
        if (stored->second != computed) {
            m.errors.push_back("[" + name + "] hash mismatch: file " + format_hash(stored->second) + ", content " +
                               format_hash(computed));
            ok = false;
        }
    }
    return ok;
}

bool seal_file(const std::string& path, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error) *error = "cannot read manifest " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    input.close();
    const std::string text = buffer.str();
    const Manifest parsed = parse_text(text, path);
    if (!parsed.errors.empty()) {
        if (error) *error = parsed.errors.front();
        return false;
    }

    std::vector<std::string> lines;
    {
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line)) lines.push_back(line);
    }

    std::vector<std::string> output;
    output.reserve(lines.size() + 8);
    std::string current;
    bool pending_hash = false;
    auto flush_pending = [&]() {
        if (!pending_hash) return;
        const auto computed = parsed.computed_hashes.find(current);
        if (computed != parsed.computed_hashes.end()) output.push_back("hash = " + format_hash(computed->second));
        pending_hash = false;
    };

    for (const std::string& line : lines) {
        const std::string bare = trim(strip_comment(line));
        if (!bare.empty() && bare.front() == '[' && bare.back() == ']') {
            flush_pending();
            output.push_back(line);
            const std::string name = lower(trim(std::string_view(bare).substr(1, bare.size() - 2)));
            // Insert a hash line for a hashed section that never had one.
            // An ignored `hash = Undefined` line is preserved as written.
            if (is_hashed_section(name) && !parsed.stored_hashes.contains(name) &&
                !parsed.ignored_hashes.contains(name)) {
                current = name;
                pending_hash = true;
            } else {
                current = name;
            }
            continue;
        }
        const auto equals = bare.find('=');
        if (equals != std::string::npos && lower(trim(std::string_view(bare).substr(0, equals))) == "hash") {
            const auto computed = parsed.computed_hashes.find(current);
            if (computed != parsed.computed_hashes.end()) {
                output.push_back("hash = " + format_hash(computed->second));
                continue;
            }
        }
        output.push_back(line);
    }
    flush_pending();

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        if (error) *error = "cannot write manifest " + path;
        return false;
    }
    for (std::size_t i = 0; i < output.size(); ++i) {
        file << output[i];
        if (i + 1 < output.size()) file << '\n';
    }
    file << '\n';
    return static_cast<bool>(file);
}

std::string file_sha256(const std::string& path, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "cannot read " + path;
        return {};
    }
    Sha256 sha;
    std::array<char, 1 << 16> buffer{};
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = file.gcount();
        if (got > 0) sha.update(reinterpret_cast<const uint8_t*>(buffer.data()), static_cast<std::size_t>(got));
    }
    return to_hex(sha.finish());
}

bool verify_image(const Manifest& m, const std::string& rpx_path, std::vector<std::string>& problems) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(rpx_path, ec);
    if (ec) {
        problems.push_back("cannot stat image " + rpx_path);
        return false;
    }
    const uint32_t expected_size = m.identity_u32("image_size");
    if (expected_size && size != expected_size)
        problems.push_back("image size mismatch: image " + std::to_string(size) + " bytes, manifest " +
                           std::to_string(expected_size));
    const std::string expected_hash = lower(m.identity_string("image_sha256"));
    if (!expected_hash.empty()) {
        std::string error;
        const std::string actual = file_sha256(rpx_path, &error);
        if (actual.empty())
            problems.push_back(error.empty() ? "cannot hash image" : error);
        else if (actual != expected_hash)
            problems.push_back("image sha256 mismatch: image " + actual + ", manifest " + expected_hash);
    }
    return problems.empty();
}

std::vector<std::string> discover_manifests(const std::vector<std::string>& roots) {
    std::vector<std::string> found;
    for (const std::string& root_text : roots) {
        if (root_text.empty()) continue;
        std::error_code ec;
        const std::filesystem::path root(root_text);
        if (!std::filesystem::is_directory(root, ec)) continue;

        // A root is normally the repository (so <root>/Manifest), but accepting
        // the manifest directory itself makes --manifest-dir convenient.
        std::vector<std::filesystem::path> directories{root / "Manifest"};
        if (lower(root.filename().string()) == "manifest") directories.push_back(root);
        for (const auto& directory : directories) {
            ec.clear();
            if (!std::filesystem::is_directory(directory, ec)) continue;
            std::filesystem::directory_iterator entry(directory, ec);
            const std::filesystem::directory_iterator end;
            for (; !ec && entry != end; entry.increment(ec)) {
                std::error_code file_ec;
                if (!entry->is_regular_file(file_ec)) continue;
                if (lower(entry->path().extension().string()) != ".manifest") continue;
                std::error_code canonical_ec;
                const auto canonical = std::filesystem::weakly_canonical(entry->path(), canonical_ec);
                found.push_back((canonical_ec ? entry->path() : canonical).string());
            }
        }
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
}

namespace {

// One line describing what a manifest claims to be, for selection diagnostics.
std::string describe_identity(const Manifest& m) {
    std::string text = m.id.empty() ? "(no id)" : m.id;
    std::vector<std::string> tags;
    for (const char* key : {"title_id", "product_code", "region"}) {
        const std::string value = m.identity_string(key);
        if (!value.empty()) tags.push_back(value);
    }
    if (!tags.empty()) {
        text += " [";
        for (std::size_t i = 0; i < tags.size(); ++i) {
            if (i) text += " ";
            text += tags[i];
        }
        text += "]";
    }
    const std::string size = m.identity_string("image_size");
    if (!size.empty()) text += " size=" + size;
    const std::string sha = lower(m.identity_string("image_sha256"));
    if (!sha.empty()) text += " sha256=" + sha;
    return text;
}

} // namespace

std::string identity_summary(const Manifest& m) {
    return describe_identity(m);
}

std::vector<ImageMatch> match_image(const std::string& rpx_path, const std::vector<std::string>& manifest_paths,
                                    std::vector<std::string>* problems) {
    std::vector<ImageMatch> matches;
    std::error_code ec;
    const auto image_size = std::filesystem::file_size(rpx_path, ec);
    if (ec) {
        if (problems) problems->push_back("cannot stat image " + rpx_path);
        return matches;
    }
    std::string hash_error;
    const std::string image_sha = lower(file_sha256(rpx_path, &hash_error));
    if (image_sha.empty()) {
        if (problems) problems->push_back(hash_error.empty() ? "cannot hash image " + rpx_path : hash_error);
        return matches;
    }

    for (const std::string& path : manifest_paths) {
        std::string parse_error;
        auto parsed = parse_file(path, &parse_error);
        if (!parsed) {
            if (problems) problems->push_back(parse_error);
            continue;
        }
        ImageMatch match;
        match.size_known = parsed->identity_value("image_size") != nullptr;
        match.size_matches = match.size_known && image_size == parsed->identity_u32("image_size");
        const std::string expected = lower(parsed->identity_string("image_sha256"));
        match.sha_known = !expected.empty();
        match.sha_matches = match.sha_known && expected == image_sha;
        match.manifest = std::move(*parsed);
        matches.push_back(std::move(match));
    }
    return matches;
}

std::optional<Manifest> select_for_image_from(const std::string& rpx_path,
                                              const std::vector<std::string>& manifest_paths, std::string* error,
                                              std::vector<std::string>* notes) {
    std::vector<std::string> problems;
    std::vector<ImageMatch> matches = match_image(rpx_path, manifest_paths, &problems);

    int best = 0;
    for (const ImageMatch& match : matches) {
        if (match.exact())
            best = std::max(best, 2);
        else if (match.size_only())
            best = std::max(best, 1);
    }
    std::vector<ImageMatch*> winners;
    for (ImageMatch& match : matches) {
        if ((best == 2 && match.exact()) || (best == 1 && match.size_only())) winners.push_back(&match);
    }

    if (notes) {
        for (const ImageMatch& match : matches) {
            std::string line = match.manifest.path + ": " + describe_identity(match.manifest);
            if (match.exact())
                line += " [matches]";
            else if (match.size_only())
                line += " [size-only match]";
            else
                line += " [does not match]";
            notes->push_back(std::move(line));
        }
        for (const std::string& problem : problems) notes->push_back(problem);
    }

    if (best > 0 && winners.size() == 1) return std::move(winners.front()->manifest);

    if (error) {
        std::error_code ec;
        const auto image_size = std::filesystem::file_size(rpx_path, ec);
        std::string image_sha;
        if (!ec) image_sha = file_sha256(rpx_path, nullptr);
        if (best == 0) {
            *error = "no manifest describes " + rpx_path;
        } else {
            *error = "several manifests describe " + rpx_path + "; pass --manifest <file> to choose one";
        }
        *error += " (image size " + std::to_string(ec ? 0 : image_size) + " bytes, sha256 " +
                  (image_sha.empty() ? "<unavailable>" : image_sha) + ")";
        if (manifest_paths.empty()) {
            *error += "\nsearched for Manifest/*.manifest under the working directory, the "
                      "executable directory and any --manifest-dir";
        }
        for (const ImageMatch& match : matches)
            *error += "\n  " + match.manifest.path + ": " + describe_identity(match.manifest);
        for (const std::string& problem : problems) *error += "\n  " + problem;
        *error += "\nadd a manifest for this image, or pass --manifest <file>";
    }
    return std::nullopt;
}

std::optional<Manifest> select_for_image(const std::string& rpx_path, const std::vector<std::string>& roots,
                                         std::string* error, std::vector<std::string>* notes) {
    const std::vector<std::string> paths = discover_manifests(roots);
    if (paths.empty()) {
        if (error) {
            *error = "no manifest found; looked for Manifest/*.manifest under:";
            for (const std::string& root : roots) *error += "\n  " + root;
            *error += "\npass --manifest <file> or --manifest-dir <directory>";
        }
        return std::nullopt;
    }
    return select_for_image_from(rpx_path, paths, error, notes);
}

std::vector<Import> imports_by_address(const Manifest& m) {
    std::vector<Import> sorted = m.imports;
    std::sort(sorted.begin(), sorted.end(), [](const Import& a, const Import& b) {
        if (a.address != b.address) return a.address < b.address;
        if (a.module != b.module) return a.module < b.module;
        return a.symbol < b.symbol;
    });
    sorted.erase(std::unique(sorted.begin(), sorted.end(),
                             [](const Import& a, const Import& b) { return a.address == b.address; }),
                 sorted.end());
    return sorted;
}

std::vector<std::string> header_paths(const Manifest& m) {
    std::vector<std::string> paths;
    paths.reserve(m.modules.size());
    for (const auto& [module, header] : m.modules) {
        (void)module;
        const std::string path = trim(header);
        if (path.empty()) continue;
        if (std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path);
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

namespace {

bool write_text_file(const std::string& path, const std::string& text, std::string* error) {
    std::error_code ec;
    const std::filesystem::path fs_path(path);
    if (fs_path.has_parent_path()) std::filesystem::create_directories(fs_path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    file << text;
    return static_cast<bool>(file);
}

// Keeps a regenerated file byte-identical when nothing changed, so the build
// does not rebuild generated objects for a no-op manifest edit.
bool write_if_changed(const std::string& path, const std::string& text, std::string* error) {
    std::ifstream existing(path, std::ios::binary);
    if (existing) {
        std::ostringstream buffer;
        buffer << existing.rdbuf();
        if (buffer.str() == text) return true;
    }
    return write_text_file(path, text, error);
}

} // namespace

bool emit(const Manifest& m, const std::string& out_dir, std::vector<std::string>& written, std::string* error) {
    const std::filesystem::path root(out_dir);
    // The banner cites the manifest id and file name, never the invocation
    // path, so the same manifest always yields byte-identical output.
    const std::string banner = "// AUTO-GENERATED from manifest '" + m.id + "' (" +
                               std::filesystem::path(m.path).filename().string() +
                               ") — DO NOT EDIT\n"
                               "// Source of truth: the manifest. Regenerate rather than edit this file.\n";

    // ---- address headers -------------------------------------------------
    for (const std::string& relative : header_paths(m)) {
        std::vector<std::string> modules;
        for (const auto& [module, header] : m.modules)
            if (trim(header) == relative) modules.push_back(module);
        std::sort(modules.begin(), modules.end());

        std::ostringstream out;
        out << banner << "#pragma once\n";
        for (const std::string& module : modules) {
            std::vector<const Import*> symbols;
            for (const Import& import : m.imports)
                if (import.module == module) symbols.push_back(&import);
            if (symbols.empty()) continue;
            std::sort(symbols.begin(), symbols.end(), [](const Import* a, const Import* b) {
                if (a->address != b->address) return a->address < b->address;
                return a->symbol < b->symbol;
            });
            out << "\n// --- " << module << " ---\n";
            for (const Import* symbol : symbols)
                out << "#define ADDR_" << symbol->symbol << ' ' << hex_address(symbol->address) << "u\n";
        }
        const std::string path = (root / relative).string();
        if (!write_if_changed(path, out.str(), error)) return false;
        written.push_back(relative);
    }

    // PLT stub registration table
    {
        std::ostringstream out;
        out << banner << "// Registers STUB for every PLT address so no call goes unhandled.\n"
            << "// Module-specific register() functions run afterwards and override\n"
            << "// the entries they implement.\n"
            << "#include \"os_common.h\"\n\n"
            << "void gambit_plt_auto_register(CPUState* cpu) {\n";
        for (const Import& import : imports_by_address(m)) {
            if (import.data) continue; // a data slot is not a callable thunk
            out << "    rbrew_register_func(cpu, " << hex_address(import.address) << "u, STUB);  // " << import.module
                << "::" << import.symbol << '\n';
        }
        out << "}\n";
        const std::string relative = "os/gambit_plt_auto.cpp";
        const std::string path = (root / relative).string();
        if (!write_if_changed(path, out.str(), error)) return false;
        written.push_back(relative);
    }
    return true;
}

} // namespace rebrewu::manifest
