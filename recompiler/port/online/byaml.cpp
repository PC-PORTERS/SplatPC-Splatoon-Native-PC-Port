#include "byaml.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace rbrew_native {
namespace {

using json = nlohmann::ordered_json;
enum : uint8_t {
    STRING = 0xA0,
    ARRAY = 0xC0,
    DICT = 0xC1,
    TABLE = 0xC2,
    BOOL = 0xD0,
    INT = 0xD1,
    FLOAT = 0xD2,
    NUL = 0xFF
};

void put32(std::string& out, size_t at, uint32_t value) {
    out[at] = char(value >> 24);
    out[at + 1] = char(value >> 16);
    out[at + 2] = char(value >> 8);
    out[at + 3] = char(value);
}

void append32(std::string& out, uint32_t value) {
    out.append(4, '\0');
    put32(out, out.size() - 4, value);
}

void append_header(std::string& out, uint8_t kind, size_t count) {
    out += char(kind);
    out += char(count >> 16);
    out += char(count >> 8);
    out += char(count);
}

void collect(const json& node, std::set<std::string>& keys, std::set<std::string>& strings) {
    if (node.is_object()) {
        for (const auto& [key, value] : node.items()) {
            keys.insert(key);
            collect(value, keys, strings);
        }
    } else if (node.is_array()) {
        for (const auto& value : node) collect(value, keys, strings);
    } else if (node.is_string()) {
        strings.insert(node.get<std::string>());
    }
}

std::string table(const std::vector<std::string>& items) {
    if (items.empty()) return {};
    std::string blob;
    append_header(blob, TABLE, items.size());
    uint32_t position = uint32_t(4 + 4 * (items.size() + 1));
    for (const auto& item : items) {
        append32(blob, position);
        position += uint32_t(item.size() + 1);
    }
    append32(blob, position);
    for (const auto& item : items) {
        blob += item;
        blob += '\0';
    }
    blob.append((4 - blob.size() % 4) % 4, '\0');
    return blob;
}

class Writer {
public:
    Writer(std::vector<std::string> keys, std::vector<std::string> strings) {
        for (size_t i = 0; i < keys.size(); ++i) key_index_[keys[i]] = uint32_t(i);
        for (size_t i = 0; i < strings.size(); ++i) string_index_[strings[i]] = uint32_t(i);
    }
    std::string out;

    // Writes a container (its children after it) and returns its offset.
    uint32_t write(const json& node) {
        const size_t start = out.size();
        if (node.is_object()) {
            std::vector<std::pair<uint32_t, const json*>> items;
            for (const auto& [key, value] : node.items()) items.emplace_back(key_index_.at(key), &value);
            std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            append_header(out, DICT, items.size());
            out.append(8 * items.size(), '\0');
            for (size_t i = 0; i < items.size(); ++i) {
                auto [kind, raw] = scalar(*items[i].second);
                if (kind == ARRAY || kind == DICT) raw = write(*items[i].second);
                put32(out, start + 4 + 8 * i, items[i].first << 8 | kind);
                put32(out, start + 8 + 8 * i, raw);
            }
        } else {
            const size_t count = node.size(), padded = (count + 3) & ~size_t(3);
            append_header(out, ARRAY, count);
            for (const auto& value : node) out += char(scalar(value).first);
            out.append(padded - count + 4 * count, '\0');
            const size_t base = start + 4 + padded;
            size_t i = 0;
            for (const auto& value : node) {
                auto [kind, raw] = scalar(value);
                if (kind == ARRAY || kind == DICT) raw = write(value);
                put32(out, base + 4 * i++, raw);
            }
        }
        return uint32_t(start);
    }

private:
    std::pair<uint8_t, uint32_t> scalar(const json& value) const {
        if (value.is_boolean()) return {BOOL, value.get<bool>() ? 1u : 0u};
        if (value.is_number_unsigned()) {
            const auto v = value.get<uint64_t>();
            if (v > 0x7FFFFFFFull) throw std::invalid_argument("int out of range");
            return {INT, uint32_t(v)};
        }
        if (value.is_number_integer()) {
            const auto v = value.get<int64_t>();
            if (v < INT32_MIN || v > INT32_MAX) throw std::invalid_argument("int out of range");
            return {INT, uint32_t(int32_t(v))};
        }
        if (value.is_number_float()) {
            const float f = float(value.get<double>());
            uint32_t raw;
            std::memcpy(&raw, &f, 4);
            return {FLOAT, raw};
        }
        if (value.is_string()) return {STRING, string_index_.at(value.get<std::string>())};
        if (value.is_null()) return {NUL, 0};
        if (value.is_array()) return {ARRAY, 0};
        if (value.is_object()) return {DICT, 0};
        throw std::invalid_argument("value BYAML cannot store");
    }

    std::map<std::string, uint32_t> key_index_, string_index_;
};

} // namespace

std::string byaml_dump(const json& root) {
    std::set<std::string> key_set, string_set; // std::string order is the byte order Python sorts by
    collect(root, key_set, string_set);
    const std::vector<std::string> keys(key_set.begin(), key_set.end()), strings(string_set.begin(), string_set.end());
    Writer writer(keys, strings);
    std::string& out = writer.out;
    out = std::string("BY") + std::string("\0\1", 2) + std::string(12, '\0');
    const std::string key_blob = table(keys), string_blob = table(strings);
    if (!key_blob.empty()) {
        put32(out, 4, uint32_t(out.size()));
        out += key_blob;
    }
    if (!string_blob.empty()) {
        put32(out, 8, uint32_t(out.size()));
        out += string_blob;
    }
    if (!root.is_null()) {
        if (!root.is_object() && !root.is_array()) throw std::invalid_argument("root must be a dict or a list");
        put32(out, 12, uint32_t(out.size()));
        writer.write(root);
    }
    return std::move(writer.out);
}

} // namespace rbrew_native
