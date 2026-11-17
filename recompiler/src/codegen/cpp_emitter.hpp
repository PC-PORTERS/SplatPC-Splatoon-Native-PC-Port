#pragma once
#include "naming.hpp"
#include "../ir/ir_module.hpp"
#include "../core/rpx/rpx_types.hpp"
#include "../core/linker/linker.hpp"
#include <ostream>
#include <filesystem>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rebrewu::codegen {
struct EmitConfig {
    bool emit_comments{false};         // include source-address comments
    bool emit_guest_addr_labels{true}; // label every guest instruction
    bool verbose{false};
    bool emit_data_sections{true};
    bool emit_ppc_image{false}; // independent original-code fallback image
    bool use_goto{true};        // use goto for block jumps (vs setjmp)
    uint32_t functions_per_file{
        250}; // max functions per shard (~500 KB/shard, optimal balance avoiding MSVC RAM & SSA thrashing while minimizing process startup overhead)
    uint32_t max_instructions_per_shard{25000}; // max IR instructions per shard
    uint32_t isolate_instruction_threshold{
        2800}; // isolate monster functions into dedicated shards to prevent stragglers
    std::string runtime_header{"rebrewu_runtime.h"};
    bool emit_coverage{false};
    // Entries whose callers reach a wrapper that runs a registered host hook
    // (rbrew_guest_hook) or, with none registered, the original body.
    std::set<uint32_t> hooked;
};

class CppEmitter {
public:
    CppEmitter(const ir::IRModule& module, const rpx::RpxModule& rpx_module, const linker::Linker& linker,
               NamingContext names, EmitConfig cfg = {});

    // Emit entire module to a directory
    // Produces: <outdir>/<module_name>.cpp, <outdir>/<module_name>.h
    bool emit(const std::filesystem::path& outdir);

    // Emit a single function to a stream
    void emit_function(const ir::IRFunction& func, std::ostream& out);

    // Emit module header (declarations)
    void emit_header(std::ostream& out);

    // Emit module data (static arrays)
    void emit_data(std::ostream& out);

    std::string_view last_error() const { return m_last_error; }

private:
    void emit_file_prologue(std::ostream& out);
    bool emit_ppc_image(const std::filesystem::path& outdir);
    void emit_block(const ir::BasicBlock& block, const ir::IRFunction& func, std::ostream& out, int indent);
    void emit_instr(const ir::IRInstr& instr, std::ostream& out, int indent);
    std::string format_operand(const ir::IROperand& op) const;
    std::string format_vreg(const ir::VReg& vr) const;

    // Small single-block leaf functions get a static inline copy in
    // <module>_inline.h; direct call sites use it so the host compiler can
    // inline away the call, prologue and guest-register spills.
    bool is_inline_candidate(const ir::IRFunction& func) const;
    void collect_inline_candidates();
    void emit_inline_header(std::ostream& out);
    std::string inline_name(uint32_t addr) const;
    std::string direct_call(uint32_t addr) const;
    void declare_temps(const ir::IRFunction& func, std::ostream& out, int indent);

    const ir::IRModule& m_ir;
    const rpx::RpxModule& m_rpx;
    const linker::Linker& m_linker;
    NamingContext m_names;
    EmitConfig m_cfg;
    std::string m_last_error;
    std::set<uint32_t> m_inline_candidates{};
    // Transient state valid only during emit_function()
    const ir::IRFunction* m_current_func{nullptr};
    std::set<uint32_t> m_fp_temps{};
    std::unordered_map<uint32_t, uint32_t> m_temp_map{};
    std::unordered_map<uint32_t, std::string> m_inline_temps{};
    std::unordered_set<const ir::IRInstr*> m_skip_instrs{};
};
} // namespace rebrewu::codegen
