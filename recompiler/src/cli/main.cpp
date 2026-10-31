#include <algorithm>
#include <fstream>
#include "cli_options.hpp"
#include "../core/rpx/rpx_loader.hpp"
#include "../core/rpl/rpl_loader.hpp"
#include "../core/linker/linker.hpp"
#include "../core/relocation/reloc_processor.hpp"
#include "../analysis/function_discovery.hpp"
#include "../core/manifest/manifest.hpp"
#include "../patches/manifest_patches.hpp"
#include "../analysis/cfg_builder.hpp"
#include "../codegen/cpp_emitter.hpp"
#include "../codegen/naming.hpp"
#include "../config/config.hpp"
#include "../diagnostics/diagnostics.hpp"
#include "rebrewu/logger.hpp"
#include <iostream>
#include <cstdlib>
#include "audit.hpp"

using namespace rebrewu;

namespace {

// The guest-specific data lives in manifests, never in this file. Every
// `Manifest/*.manifest` is a candidate and the image itself selects the right
// one, so a new version or region only needs a new file next to the others.
std::vector<std::string> manifest_roots(const cli::CliOptions& opts, const char* argv0) {
    std::vector<std::string> roots;
    for (const auto& directory : opts.manifest_dirs) roots.push_back(directory.string());
    if (const char* env = std::getenv("RBREW_MANIFEST_DIR"); env && *env) roots.emplace_back(env);

    std::error_code ec;
    auto add_ancestors = [&roots](std::filesystem::path directory) {
        for (int depth = 0; depth < 5 && !directory.empty(); ++depth) {
            roots.push_back(directory.string());
            const auto parent = directory.parent_path();
            if (parent == directory) break;
            directory = parent;
        }
    };
    const auto cwd = std::filesystem::current_path(ec);
    if (!ec) add_ancestors(cwd);
    if (argv0 && *argv0) {
        const auto exe = std::filesystem::absolute(argv0, ec);
        if (!ec) add_ancestors(exe.parent_path());
    }

    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    return roots;
}

// Loads --manifest when given, otherwise lets the RPX pick a manifest. Without
// an RPX, a single discovered manifest is used, and several are an error.
std::optional<manifest::Manifest> load_manifest(const cli::CliOptions& opts, const char* argv0,
                                                std::string* path_used) {
    if (!opts.manifest_path.empty()) {
        std::string error;
        auto parsed = manifest::parse_file(opts.manifest_path.string(), &error);
        if (!parsed) {
            RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << error << '\n';
            return std::nullopt;
        }
        *path_used = opts.manifest_path.string();
        return parsed;
    }

    const std::vector<std::string> roots = manifest_roots(opts, argv0);
    if (!opts.rpx_path.empty()) {
        std::string error;
        std::vector<std::string> notes;
        auto selected = manifest::select_for_image(opts.rpx_path.string(), roots, &error, &notes);
        if (!selected) {
            RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << error << '\n';
            return std::nullopt;
        }
        if (opts.verbose)
            for (const auto& note : notes)
                RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << "manifest: " << note << '\n';
        *path_used = selected->path;
        return selected;
    }

    const std::vector<std::string> paths = manifest::discover_manifests(roots);
    if (paths.size() == 1) {
        std::string error;
        auto parsed = manifest::parse_file(paths.front(), &error);
        if (!parsed) {
            RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << error << '\n';
            return std::nullopt;
        }
        *path_used = paths.front();
        return parsed;
    }
    if (paths.empty()) {
        RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr)
            << "no manifest found; pass --manifest <file> or --manifest-dir <directory>\n";
        return std::nullopt;
    }
    RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr)
        << paths.size() << " manifests found; pass --manifest <file> or --rpx <image> to choose one:\n";
    for (const auto& path : paths) RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << "  " << path << '\n';
    return std::nullopt;
}

int list_manifests(const cli::CliOptions& opts, const char* argv0) {
    const std::vector<std::string> roots = manifest_roots(opts, argv0);
    const std::vector<std::string> paths = manifest::discover_manifests(roots);
    if (paths.empty()) {
        RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr)
            << "no manifest found; looked for Manifest/*.manifest under:\n";
        for (const auto& root : roots) RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << "  " << root << '\n';
        return EXIT_FAILURE;
    }
    if (!opts.rpx_path.empty()) {
        std::vector<std::string> problems;
        const auto matches = manifest::match_image(opts.rpx_path.string(), paths, &problems);
        for (const auto& match : matches) {
            const char* status = match.exact() ? "MATCH" : match.size_only() ? "size-only" : "no";
            RBREW_LOG_STREAM(::rbrew::log::Level::Info, nullptr)
                << status << "  " << match.manifest.path << "  " << manifest::identity_summary(match.manifest) << '\n';
        }
        for (const auto& problem : problems)
            RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << "manifest: " << problem << '\n';
        return matches.empty() ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    for (const std::string& path : paths) {
        std::string error;
        auto parsed = manifest::parse_file(path, &error);
        if (!parsed) {
            RBREW_LOG_STREAM(::rbrew::log::Level::Info, nullptr) << "unreadable  " << path << "  (" << error << ")\n";
            continue;
        }
        RBREW_LOG_STREAM(::rbrew::log::Level::Info, nullptr)
            << "candidate  " << path << "  " << manifest::identity_summary(*parsed) << '\n';
    }
    return EXIT_SUCCESS;
}

int run_manifest_command(const cli::CliOptions& opts, const char* argv0) {
    const std::string action = opts.manifest_action.empty() ? "verify" : opts.manifest_action;
    if (action == "list") return list_manifests(opts, argv0);

    std::string path;
    auto parsed = load_manifest(opts, argv0, &path);
    if (!parsed) return EXIT_FAILURE;
    std::string error;

    if (action == "seal") {
        if (!parsed->errors.empty()) {
            for (const auto& e : parsed->errors)
                RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << "manifest: " << e << '\n';
            return EXIT_FAILURE;
        }
        if (!manifest::seal_file(path, &error)) {
            RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << error << '\n';
            return EXIT_FAILURE;
        }
        auto resealed = manifest::parse_file(path, &error);
        if (!resealed) {
            RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << error << '\n';
            return EXIT_FAILURE;
        }
        RBREW_LOG_STREAM(::rbrew::log::Level::Info, nullptr)
            << "Sealed " << path << " (" << resealed->hashed_section_count() << " hashed sections)\n";
        return EXIT_SUCCESS;
    }

    if (action == "emit") {
        if (!manifest::verify_hashes(*parsed)) {
            for (const auto& e : parsed->errors)
                RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << "manifest: " << e << '\n';
            RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr)
                << "manifest: refusing to emit from an unsealed manifest\n";
            return EXIT_FAILURE;
        }
        const std::string out =
            opts.manifest_emit_dir.empty() ? std::string("recompiler/port") : opts.manifest_emit_dir.string();
        std::vector<std::string> written;
        if (!manifest::emit(*parsed, out, written, &error)) {
            RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << error << '\n';
            return EXIT_FAILURE;
        }
        for (const auto& file : written)
            RBREW_LOG_STREAM(::rbrew::log::Level::Info, nullptr)
                << "Wrote " << (std::filesystem::path(out) / file).string() << '\n';
        return EXIT_SUCCESS;
    }

    if (action == "show") {
        const auto corroborating = std::count_if(parsed->entries.begin(), parsed->entries.end(),
                                                 [](const manifest::Entry& entry) { return entry.corroborate; });
        RBREW_LOG_STREAM(::rbrew::log::Level::Info, nullptr)
            << "Manifest:   " << path << '\n'
            << "Id:         " << parsed->id << '\n'
            << "Title:      " << parsed->identity_string("title_id") << " " << parsed->identity_string("product_code")
            << " (" << parsed->identity_string("region") << ")\n"
            << "App version:" << ' ' << parsed->identity_string("app_version") << '\n'
            << "Entry point: " << manifest::format_address(parsed->identity_u32("entry_point")) << '\n'
            << "Modules:    " << parsed->modules.size() << '\n'
            << "Imports:    " << parsed->imports.size() << '\n'
            << "Entries:    " << parsed->entries.size() << " (" << parsed->entries.size() - corroborating << " hints, "
            << corroborating << " corroborating)\n"
            << "Hooks:      " << parsed->hooks.size() << '\n'
            << "Patches:    " << parsed->patches.size() << '\n'
            << "Sections:   " << parsed->hashed_section_count() << " hashed\n";
        return EXIT_SUCCESS;
    }

    if (action != "verify") {
        RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr)
            << "unknown manifest action '" << action << "' (expected list, verify, seal, emit or show)\n";
        return EXIT_FAILURE;
    }

    const bool hashes_ok = manifest::verify_hashes(*parsed);
    for (const auto& e : parsed->errors)
        RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << "manifest: " << e << '\n';
    for (const auto& w : parsed->warnings)
        RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << "manifest: warning: " << w << '\n';

    bool patches_ok = true;
    std::vector<std::string> patch_errors;
    if (!opts.rpx_path.empty()) {
        diagnostics::DiagEngine diag(opts.verbose);
        rpx::RpxLoader loader(diag);
        auto image = loader.load(opts.rpx_path);
        if (!image) {
            patch_errors.push_back("cannot load image " + opts.rpx_path.string());
            patches_ok = false;
        } else {
            patches_ok = patches::validate(*parsed, *image, patch_errors);
        }
    } else {
        std::vector<patches::EncodedPatch> encoded_patches;
        patches_ok = patches::encode(*parsed, encoded_patches, patch_errors);
    }
    for (const auto& e : patch_errors)
        RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << "manifest: " << e << '\n';

    bool images_ok = true;
    if (!opts.rpx_path.empty()) {
        std::vector<std::string> problems;
        images_ok = manifest::verify_image(*parsed, opts.rpx_path.string(), problems);
        for (const auto& problem : problems)
            RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << "manifest: " << problem << '\n';
    }
    if (!hashes_ok || !patches_ok || !images_ok) return EXIT_FAILURE;
    RBREW_LOG_STREAM(::rbrew::log::Level::Info, nullptr)
        << "Manifest OK: " << parsed->id << " (" << parsed->hashed_section_count() << " hashed sections, "
        << parsed->imports.size() << " imports, " << parsed->entries.size() << " entries, " << parsed->hooks.size()
        << " hooks, " << parsed->patches.size() << " patches)\n";
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    auto opts = cli::parse(argc, argv);
    if (!opts) return EXIT_FAILURE;

    if (opts->command == cli::Command::Manifest) return run_manifest_command(*opts, argv[0]);

    diagnostics::DiagEngine diag(opts->verbose);

    rpx::RpxLoader rpx_loader(diag);
    auto rpx_opt = rpx_loader.load(opts->rpx_path);
    if (!rpx_opt) {
        RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << "Failed to load RPX: " << opts->rpx_path << "\n";
        return EXIT_FAILURE;
    }
    auto& rpx = *rpx_opt;

    linker::Linker lnk(diag);
    rpl::RplLoader rpl_loader(diag);
    for (const auto& rpl_path : opts->rpl_paths) {
        auto rpl_opt = rpl_loader.load(rpl_path);
        if (!rpl_opt) {
            RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr)
                << "warning: failed to load RPL: " << rpl_path << "\n";
            continue;
        }
        lnk.add_rpl(std::make_shared<rpl::RplModule>(std::move(*rpl_opt)));
    }
    // The host runtime replaces Cafe OS RPLs; inspection does not require them.
    if (!opts->rpl_paths.empty() && !lnk.link(rpx)) return EXIT_FAILURE;

    if (opts->command == cli::Command::Inspect) {
        RBREW_LOG_STREAM(::rbrew::log::Level::Info, nullptr)
            << "Module: " << rpx.name << "\nEntry point: 0x" << std::hex << rpx.entry_point << "\nSDA: 0x"
            << rpx.file_info.sda_base << "\nSDA2: 0x" << rpx.file_info.sda2_base << std::dec
            << "\nSections: " << rpx.sections.size() << "\nSymbols: " << rpx.symbols.size()
            << "\nImports: " << rpx.imports.size() << "\nExports: " << rpx.exports.size()
            << "\nRelocations: " << rpx.relocations.size() << "\n";
        return EXIT_SUCCESS;
    }

    if (opts->command == cli::Command::ListSymbols) {
        for (const auto& sym : rpx.symbols)
            RBREW_LOG_STREAM(::rbrew::log::Level::Info, nullptr) << std::hex << sym.address << "  " << sym.name << "\n";
        return EXIT_SUCCESS;
    }

    if (opts->command == cli::Command::ListSections) {
        for (const auto& sec : rpx.sections)
            RBREW_LOG_STREAM(::rbrew::log::Level::Info, nullptr)
                << sec.name << "  addr=0x" << std::hex << sec.address << "  size=0x" << sec.size << "\n";
        return EXIT_SUCCESS;
    }

    if (opts->command == cli::Command::ListExports) {
        for (const auto& exp : rpx.exports)
            RBREW_LOG_STREAM(::rbrew::log::Level::Info, nullptr) << std::hex << exp.address << "  " << exp.name << "\n";
        return EXIT_SUCCESS;
    }

    if (opts->command == cli::Command::ListImports) {
        for (const auto& imp : rpx.imports)
            RBREW_LOG_STREAM(::rbrew::log::Level::Info, nullptr)
                << imp.from_module << "!" << imp.name << "  stub=0x" << std::hex << imp.stub_address << "\n";
        return EXIT_SUCCESS;
    }

    if (opts->no_codegen) return EXIT_SUCCESS;

    // Function discovery
    analysis::FunctionDiscovery::Config disc_cfg;
    AuditInventory audit;
    if (opts->command == cli::Command::Audit) disc_cfg.frame_audit = [&](const auto& finding) { audit.frame(finding); };
    if (opts->command == cli::Command::Audit) disc_cfg.thunk_audit = [&](const auto& finding) { audit.thunk(finding); };
    // External analyzer function starts (e.g. Ghidra "NAME at ADDR" export):
    // corroborating evidence only, combined with reference and separator proofs.
    if (!opts->analyzer_entries_file.empty()) {
        std::ifstream entries(opts->analyzer_entries_file);
        if (!entries) {
            RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr)
                << "cannot read analyzer entries " << opts->analyzer_entries_file << '\n';
            return EXIT_FAILURE;
        }
        for (std::string line; std::getline(entries, line);) {
            const auto at = line.rfind(" at ");
            const std::string hex = at == std::string::npos ? line : line.substr(at + 4);
            if (hex.empty() || hex[0] == '#') continue;
            try {
                disc_cfg.analyzer_entries.insert(uint32_t(std::stoul(hex, nullptr, 16)));
            } catch (...) {}
        }
        if (opts->verbose)
            RBREW_LOG_STREAM(::rbrew::log::Level::Info, nullptr)
                << "Loaded " << disc_cfg.analyzer_entries.size() << " analyzer entries for corroboration\n";
    }
    // The manifest supplies the entry hints and the instruction patches. Both
    // must be known before discovery, because a patch can change control flow.
    std::string manifest_path;
    auto manifest_opt = load_manifest(*opts, argv[0], &manifest_path);
    if (!manifest_opt) {
        // Without a manifest the indirect-dispatch entries are unknown, which
        // silently drops reachable functions. Fail instead of guessing.
        return EXIT_FAILURE;
    }
    auto& mf = *manifest_opt;
    const bool hashes_ok = manifest::verify_hashes(mf);
    for (const auto& e : mf.errors) RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << "manifest: " << e << '\n';
    if (!hashes_ok) {
        RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr)
            << "manifest: " << manifest_path << " was modified without resealing (run 'rebrewu manifest seal')\n";
        return EXIT_FAILURE;
    }
    std::vector<std::string> problems;
    if (!manifest::verify_image(mf, opts->rpx_path.string(), problems)) {
        for (const auto& problem : problems)
            RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << "manifest: " << problem << '\n';
        if (!opts->allow_image_mismatch) {
            RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr)
                << "manifest: " << manifest_path << " does not describe " << opts->rpx_path
                << " (pass --allow-image-mismatch to recompile anyway)\n";
            return EXIT_FAILURE;
        }
        RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << "manifest: continuing despite the identity mismatch\n";
    }

    std::vector<std::string> patch_errors;
    if (!patches::apply(mf, rpx, patch_errors)) {
        for (const auto& error : patch_errors)
            RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << "manifest: " << error << '\n';
        return EXIT_FAILURE;
    }

    std::size_t hint_count = 0;
    for (const auto& entry : mf.entries) {
        if (entry.corroborate)
            disc_cfg.analyzer_entries.insert(entry.address);
        else
            ++hint_count;
    }
    analysis::FunctionDiscovery disc(rpx, lnk, disc_cfg);
    for (const auto& entry : mf.entries)
        if (!entry.corroborate) disc.add_hint({entry.address, {}, entry.force});
    if (opts->verbose)
        RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr)
            << "Manifest " << mf.id << " (" << manifest_path << "): " << mf.imports.size() << " imports, " << hint_count
            << " entry hints, " << mf.entries.size() - hint_count << " corroborating entries, " << mf.hooks.size()
            << " hooks, " << mf.patches.size() << " patches\n";
    // Diagnostic inventory only: force every listed address as an entry so a
    // single run can show which undiscovered targets startup really reaches.
    if (!opts->hints_file.empty()) {
        std::ifstream hints(opts->hints_file);
        if (!hints) {
            RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr)
                << "cannot read hints file " << opts->hints_file << '\n';
            return EXIT_FAILURE;
        }
        unsigned count = 0;
        for (std::string line; std::getline(hints, line);) {
            if (line.empty() || line[0] == '#') continue;
            disc.add_hint({uint32_t(std::stoul(line, nullptr, 16)), {}, true});
            ++count;
        }
        RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr)
            << "Forced " << count << " diagnostic entry hints from " << opts->hints_file << '\n';
    }
    auto boundaries = disc.discover();

    if (opts->verbose)
        RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr)
            << "Discovered " << boundaries.size() << " function candidates.\n"
            << "Recognized " << disc.stats().pre_stack_prologues << " pre-stack LR-save prologues.\n"
            << "Recognized " << disc.stats().post_stack_prologues
            << " post-stack LR-save prologues with validated unwinds.\n"
            << "Added " << disc.stats().from_code_ptr_relocs
            << " isolated address-taken leaves from code relocations.\n"
            << "Added " << disc.stats().from_tail_callbacks << " relocation-backed conditional tail callbacks.\n"
            << "Added " << disc.stats().from_backward_leaf_thunks
            << " relocation-backed isolated backward leaf thunks.\n";

    analysis::CFGBuilder cfg_builder(rpx);
    ir::IRModule ir_module;
    ir_module.name = rpx.name;
    ir_module.is_rpx = true;

    uint32_t built_ok = 0, built_fail = 0;
    for (const auto& fb : boundaries) {
        auto func = cfg_builder.build(fb.start, fb.name);
        if (func) {
            ir_module.functions.push_back(std::move(*func));
            ++built_ok;
        } else {
            ++built_fail;
            if (opts->verbose)
                RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr)
                    << "  build failed @ 0x" << std::hex << fb.start << ": " << cfg_builder.last_error() << "\n";
        }
    }
    if (opts->verbose)
        RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr)
            << "Built " << std::dec << built_ok << " functions (" << built_fail << " failed).\n"
            << "Entry point: 0x" << std::hex << rpx.entry_point << "\n";

    if (opts->command == cli::Command::Audit) {
        return audit.write(rpx, ir_module, cfg_builder, opts->output_dir) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (opts->command == cli::Command::Analyze) {
        RBREW_LOG_STREAM(::rbrew::log::Level::Info, nullptr)
            << "Discovered " << boundaries.size() << " function candidates.\n"
            << "Built " << built_ok << " functions (" << built_fail << " failed).\nEntry point: 0x" << std::hex
            << rpx.entry_point << "\n";
        return EXIT_SUCCESS;
    }

    for (const auto& sym : rpx.symbols) {
        if (sym.address != 0 && !sym.name.empty()) ir_module.add_symbol(sym.address, sym.name);
    }
    for (const auto& exp : rpx.exports) {
        if (exp.address != 0 && !exp.name.empty()) {
            ir_module.add_symbol(exp.address, exp.name);
            ir_module.exports[exp.name] = exp.address;
        }
    }

    for (const auto& sec : rpx.sections) {
        if (!sec.is_allocated() || sec.is_executable() || sec.data.empty()) continue;
        ir::IRDataSection ds;
        ds.name = sec.name;
        ds.guest_addr = sec.address;
        ds.data = sec.data;
        ds.read_only = !sec.is_writable();
        ir_module.data_sections.push_back(std::move(ds));
    }

    codegen::NamingContext names(rpx.name);
    codegen::EmitConfig emit_config;
    emit_config.emit_ppc_image = opts->emit_ppc_image;
    emit_config.emit_coverage = opts->emit_coverage;
    if (opts->functions_per_file.has_value()) {
        emit_config.functions_per_file = *opts->functions_per_file;
    }
    emit_config.hooked.insert(mf.hooks.begin(), mf.hooks.end());
    if (!opts->hooks_file.empty()) {
        std::ifstream hooks(opts->hooks_file);
        if (!hooks) {
            RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << "cannot read hooks " << opts->hooks_file << "\n";
            return EXIT_FAILURE;
        }
        for (std::string line; std::getline(hooks, line);) {
            const auto hex = line.substr(0, line.find_first_of("# \t\r"));
            if (hex.empty()) continue;
            try {
                emit_config.hooked.insert(uint32_t(std::stoul(hex, nullptr, 16)));
            } catch (...) {
                RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr) << "invalid hook address " << line << "\n";
                return EXIT_FAILURE;
            }
        }
    }
    for (uint32_t addr : emit_config.hooked)
        if (std::none_of(ir_module.functions.begin(), ir_module.functions.end(),
                         [&](const auto& f) { return f.entry_addr == addr; })) {
            RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr)
                << "hook address " << std::hex << addr << " is not a discovered function entry" << "\n";
            return EXIT_FAILURE;
        }
    if (!emit_config.hooked.empty())
        RBREW_LOG_STREAM(::rbrew::log::Level::Info, nullptr) << "Hook wrappers: " << emit_config.hooked.size() << "\n";
    codegen::CppEmitter emitter(ir_module, rpx, lnk, std::move(names), emit_config);
    if (!emitter.emit(opts->output_dir)) {
        RBREW_LOG_STREAM(::rbrew::log::Level::Error, nullptr)
            << "Code generation failed: " << emitter.last_error() << "\n";
        return EXIT_FAILURE;
    }

    RBREW_LOG_STREAM(::rbrew::log::Level::Info, nullptr) << "Done. Output written to " << opts->output_dir << "\n";
    return EXIT_SUCCESS;
}
