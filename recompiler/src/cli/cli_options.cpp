#include "cli_options.hpp"
#include "rebrewu/logger.hpp"
#include "rebrewu/version.hpp"
#include <cstring>
#include <iostream>

namespace rebrewu::cli {

void print_usage(const char* prog) {
    RBREW_LOG_STREAM(::rbrew::log::Level::Info, nullptr)
        << "Usage: " << prog
        << " <command> [options]\n\n"
           "Commands:\n"
           "  recompile   Recompile an RPX to C++ (default)\n"
           "  audit       Write read-only discovery/CFG/semantics JSON inventory\n"
           "  manifest    list|verify|seal|emit|show [file]  Per-image guest data\n"
           "  inspect     Inspect binary structure\n"
           "  symbols     List symbols\n"
           "  sections    List sections\n"
           "  exports     List RPL exports\n"
           "  imports     List RPL imports\n"
           "  disasm      Disassemble a range\n"
           "  help        Show this help\n"
           "  version     Show version\n\n"
           "Options:\n"
           "  -o <dir>    Output directory (default: output)\n"
           "  -l <rpl>    Add RPL search path or file\n"
           "  -c <cfg>    Config file path\n"
           "  -v          Verbose output\n"
           "  --no-color  Disable coloured output\n"
           "  --discover-referenced-entries  Prove all address-taken entry boundaries\n"
           "  --emit-ppc-image  Emit relocated executable image for hybrid prototype\n"
           "  --emit-coverage   Emit RBREW_COVERAGE_* probes (needs RBREW_ENABLE_COVERAGE=ON)\n"
           "  --analyzer-entries F  Additional analyzer starts merged with [entries] corroborate\n"
           "  --hooks F         Additional host-hook entries merged with [hooks] (hex per line)\n"
           "  -F, --functions-per-file N  Max functions per generated shard\n"
           "  --hints-file F    Diagnostic only: force extra entries (hex address per line)\n"
           "  --manifest F      Force this manifest instead of selecting one by RPX identity\n"
           "  --manifest-dir D  Add a directory searched for Manifest/*.manifest (repeatable)\n"
           "  --emit-dir D      Output root for 'manifest emit' (default recompiler/port)\n"
           "  --rpx F           Image used to select and verify the manifest\n"
           "  --allow-image-mismatch  Warn instead of failing on an identity mismatch\n";
}

void print_version() {
    RBREW_LOG_STREAM(::rbrew::log::Level::Info, nullptr) << "RebrewU v" << rebrewu::VERSION_STRING << "\n";
}

std::optional<CliOptions> parse(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return {};
    }

    CliOptions opts;
    int i = 1;

    std::string_view cmd = argv[i];
    if (cmd == "recompile") {
        opts.command = Command::Recompile;
        ++i;
    } else if (cmd == "inspect") {
        opts.command = Command::Inspect;
        ++i;
    } else if (cmd == "symbols") {
        opts.command = Command::ListSymbols;
        ++i;
    } else if (cmd == "sections") {
        opts.command = Command::ListSections;
        ++i;
    } else if (cmd == "exports") {
        opts.command = Command::ListExports;
        ++i;
    } else if (cmd == "imports") {
        opts.command = Command::ListImports;
        ++i;
    } else if (cmd == "disasm") {
        opts.command = Command::Disassemble;
        ++i;
    } else if (cmd == "audit") {
        opts.command = Command::Audit;
        ++i;
    } else if (cmd == "manifest") {
        opts.command = Command::Manifest;
        ++i;
        // list | verify | seal | emit | show, then the manifest path.
        if (i < argc && argv[i][0] != '-') {
            opts.manifest_action = argv[i];
            ++i;
            if (i < argc && argv[i][0] != '-') {
                opts.manifest_path = argv[i];
                ++i;
            }
        }
    } else if (cmd == "analyze") {
        opts.command = Command::Analyze;
        ++i;
    } else if (cmd == "help") {
        print_usage(argv[0]);
        return {};
    } else if (cmd == "version") {
        print_version();
        return {};
    }
    // else treat first arg as RPX path with default Recompile command

    for (; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-v" || arg == "--verbose") {
            opts.verbose = true;
        } else if (arg == "--no-color") {
            opts.color = false;
        } else if (arg == "--no-codegen") {
            opts.no_codegen = true;
        } else if (arg == "--rpx" && i + 1 < argc) {
            opts.rpx_path = argv[++i];
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            opts.output_dir = argv[++i];
        } else if (arg == "-l" && i + 1 < argc) {
            opts.rpl_paths.emplace_back(argv[++i]);
        } else if (arg == "-c" && i + 1 < argc) {
            opts.config_path = argv[++i];
        } else if (arg == "--analyzer-entries" && i + 1 < argc) {
            opts.analyzer_entries_file = argv[++i];
        } else if (arg == "--hooks" && i + 1 < argc) {
            opts.hooks_file = argv[++i];
        } else if (arg == "--hints-file" && i + 1 < argc) {
            opts.hints_file = argv[++i];
        } else if (arg == "--discover-referenced-entries") {
            opts.discover_referenced_entries = true;
        } else if (arg == "--emit-ppc-image") {
            opts.emit_ppc_image = true;
        } else if (arg == "--emit-coverage") {
            opts.emit_coverage = true;
        } else if (arg == "--manifest" && i + 1 < argc) {
            opts.manifest_path = argv[++i];
        } else if (arg == "--manifest-dir" && i + 1 < argc) {
            opts.manifest_dirs.emplace_back(argv[++i]);
        } else if (arg == "--emit-dir" && i + 1 < argc) {
            opts.manifest_emit_dir = argv[++i];
        } else if (arg == "--allow-image-mismatch") {
            opts.allow_image_mismatch = true;
        } else if ((arg == "--functions-per-file" || arg == "-F") && i + 1 < argc) {
            opts.functions_per_file = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg.starts_with("-")) {
            RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << "unknown option: " << arg << "\n";
            return {};
        } else {
            if (opts.rpx_path.empty()) opts.rpx_path = arg;
        }
    }

    return opts;
}

} // namespace rebrewu::cli
