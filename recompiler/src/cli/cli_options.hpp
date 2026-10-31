#pragma once
#include <string>
#include <vector>
#include <optional>
#include <filesystem>

namespace rebrewu::cli {
enum class Command {
    Recompile,    // main recompilation
    Inspect,      // inspect binary structure
    DumpRelocs,   // dump relocation tables
    ListSymbols,  // list symbols
    ListSections, // list sections
    Disassemble,  // disassemble a range
    ListExports,  // list RPL exports
    ListImports,  // list RPL imports
    Audit,        // reusable read-only discovery/CFG inventory
    Analyze,      // run analysis only (no codegen)
    Manifest,     // verify / seal / emit / show a manifest
    Help,
    Version
};

struct CliOptions {
    Command command{Command::Recompile};
    std::filesystem::path rpx_path;
    std::vector<std::filesystem::path> rpl_paths;
    std::filesystem::path output_dir{"output"};
    std::optional<std::filesystem::path> config_path;

    // Inspect options
    bool show_headers{false};
    bool show_sections{false};
    bool show_segments{false};

    // Disassemble options
    std::optional<uint32_t> disasm_start;
    std::optional<uint32_t> disasm_end;
    std::optional<std::string> disasm_function;

    // Symbol options
    bool show_imports{true};
    bool show_exports{true};
    bool show_synthetic{false};

    // General
    bool verbose{false};
    bool color{true};
    bool no_codegen{false};
    bool emit_ppc_image{false};        // relocated executable bytes for opt-in fallback
    bool emit_coverage{false};         // RBREW_COVERAGE_* probes in generated code
    std::string hints_file;            // diagnostic: extra forced entry addresses (hex per line)
    std::string analyzer_entries_file; // extra analyzer starts; merged with [entries] corroborate
    std::optional<uint32_t> functions_per_file;
    std::string hooks_file; // extra host-hook entries; merged with [hooks]

    // Manifest: per-image guest data (identity, PLT map, entry hints). When no
    // path is given, every Manifest/*.manifest is a candidate and the RPX
    // selects the one whose [identity] describes it.
    std::filesystem::path manifest_path;
    std::vector<std::filesystem::path> manifest_dirs; // extra discovery roots
    std::filesystem::path manifest_emit_dir;          // 'manifest emit' output root
    std::string manifest_action;                      // list | verify | seal | emit | show
    bool allow_image_mismatch{false};                 // downgrade identity mismatch to a warning
};

std::optional<CliOptions> parse(int argc, char** argv);

void print_usage(const char* prog);
void print_version();
} // namespace rebrewu::cli
