#pragma once
// ============================================================================
// RebrewU manifest: the per-image source of truth for guest-specific data.
//
// Replaces the hand-maintained PLT address headers and the hardcoded entry
// hint lists. One manifest describes one exact guest image (title + version +
// region + build). Adding a version or region means adding a manifest, not
// editing source.
//
// Text format "rebrew-manifest/1": sections of `key = value` lines, `#` and
// `//` comments, blank lines ignored. Every data section carries its own
// `hash` line, computed over that section's own pairs only, so appending a
// section, adding comments or reordering lines never invalidates a hash.
// ============================================================================
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace rebrewu::manifest {

inline constexpr const char* kFormatVersion = "rebrew-manifest/1";

struct Import {
    std::string module; // e.g. "coreinit"
    std::string symbol; // e.g. "FSReadFile"
    uint32_t address{}; // PLT stub address, or the slot of a data import
    // Data imports (`.dimport_*`) resolve to a guest data address, not to a
    // callable thunk, so they get an address define but never a PLT stub.
    bool data{false};
};

struct Entry {
    uint32_t address{};
    bool force{false};       // bypass is_valid_code_addr for coverage gaps
    bool corroborate{false}; // external evidence only; never a discovery hint
};

struct Patch {
    uint32_t address{};
    std::string instruction; // assembled when the manifest is applied to an image
};

struct Manifest {
    std::string path;
    std::string id;                              // [manifest] id
    std::map<std::string, std::string> meta;     // [manifest]
    std::map<std::string, std::string> identity; // [identity]
    std::map<std::string, std::string> modules;  // [modules]  module -> header path
    std::vector<Import> imports;                 // [imports]
    std::vector<Entry> entries;                  // [entries]
    std::vector<uint32_t> hooks;                 // [hooks] guest entries with host-hook wrappers
    std::vector<Patch> patches;                  // [patches]

    // section name -> hash stored in the file / recomputed from content
    std::map<std::string, uint64_t> stored_hashes;
    std::map<std::string, uint64_t> computed_hashes;
    // sections whose `hash` line is `Undefined`: never verified, never resealed
    std::set<std::string> ignored_hashes;

    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    const std::string* identity_value(const std::string& key) const;
    // Parsed identity fields with defaults; "" / 0 when absent or malformed.
    std::string identity_string(const std::string& key) const;
    uint32_t identity_u32(const std::string& key, uint32_t fallback = 0) const;

    std::size_t hashed_section_count() const { return computed_hashes.size(); }
};

// Sections whose contents are covered by a per-section hash line.
bool is_hashed_section(const std::string& name);

std::optional<Manifest> parse_file(const std::string& path, std::string* error);
Manifest parse_text(const std::string& text, const std::string& path);

// FNV-1a 64 over the canonical form of a section (sorted pairs, normalized
// values). Additions outside the section cannot change it.
uint64_t section_hash(const std::map<std::string, std::string>& pairs);

// True when every present hash line matches its section content.
bool verify_hashes(Manifest& m);
// Rewrites the `hash` line of every hashed section in place, leaving all
// comments, blank lines and pair order untouched.
bool seal_file(const std::string& path, std::string* error);

// SHA-256 of a file, lowercase hex. Empty string on read failure.
std::string file_sha256(const std::string& path, std::string* error);

// Verifies that an RPX file matches this manifest's [identity] section.
// Checks size always; checks SHA-256 when the field is present.
bool verify_image(const Manifest& m, const std::string& rpx_path, std::vector<std::string>& problems);

// ---------------------------------------------------------------------------
// Discovery and automatic selection
// ---------------------------------------------------------------------------
// Every `*.manifest` found in `<root>/Manifest` (or directly in `<root>` when
// `<root>` is itself a manifest directory). Results are sorted and de-duplicated
// by canonical path.
std::vector<std::string> discover_manifests(const std::vector<std::string>& roots);

// One-line identity (`id [title_id product_code region] size=... sha256=...`).
std::string identity_summary(const Manifest& m);

// One manifest ranked against an image. `exact` means the stored SHA-256
// matches; `size_only` is a weaker match used when the manifest carries no
// digest. A size or digest that mismatches disqualifies the candidate.
struct ImageMatch {
    Manifest manifest;
    bool size_known{false};
    bool size_matches{false};
    bool sha_known{false};
    bool sha_matches{false};

    bool exact() const { return sha_known && sha_matches; }
    bool size_only() const { return !sha_known && size_known && size_matches; }
    bool matches() const { return exact() || size_only(); }
};

// Parses every candidate and compares it against the image. The image is
// hashed once, not once per candidate. Parsing failures are reported in
// `problems`, not treated as matches.
std::vector<ImageMatch> match_image(const std::string& rpx_path, const std::vector<std::string>& manifest_paths,
                                    std::vector<std::string>* problems = nullptr);

// Chooses the manifest that describes `rpx_path`, discovering candidates under
// `roots`. An exact SHA-256 match wins over a size-only match; several matches
// of the same strength are ambiguous and fail. On failure `error` lists the
// image fingerprint and every manifest that was considered, so adding a new
// version or region is an obvious next step.
std::optional<Manifest> select_for_image(const std::string& rpx_path, const std::vector<std::string>& roots,
                                         std::string* error, std::vector<std::string>* notes = nullptr);

// Same, for an explicit candidate list (already discovered by the caller).
std::optional<Manifest> select_for_image_from(const std::string& rpx_path,
                                              const std::vector<std::string>& manifest_paths, std::string* error,
                                              std::vector<std::string>* notes = nullptr);

// Regenerates the address headers and the PLT stub table described by the
// manifest, under out_dir (i.e. recompiler/port). Returns false on I/O error
// and lists what it wrote in `written`.
bool emit(const Manifest& m, const std::string& out_dir, std::vector<std::string>& written, std::string* error);

// Sorted, de-duplicated-by-address import view used by the emitters.
std::vector<Import> imports_by_address(const Manifest& m);
// Distinct header paths declared by [modules], sorted.
std::vector<std::string> header_paths(const Manifest& m);

std::string format_hash(uint64_t value);
std::string format_address(uint32_t value);

} // namespace rebrewu::manifest
