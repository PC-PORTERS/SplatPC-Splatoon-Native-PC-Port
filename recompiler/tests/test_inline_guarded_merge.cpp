#include "analysis/cfg_builder.hpp"
#include <cstdio>
#include <stdexcept>
using namespace rebrewu;
constexpr uint32_t base=0x02000000u;
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static void put(rpx::RpxSection& s,uint32_t off,uint32_t word){for(unsigned i=0;i<4;++i)s.data[off+i]=uint8_t(word>>(24-8*i));}
static rpx::RpxModule fixture(){
 rpx::RpxModule m{};m.name="authored_inline_table";m.entry_point=base;
 rpx::RpxSection t{};t.name=".text";t.address=base;t.size=0x100;t.flags=elf::SHF_ALLOC|elf::SHF_EXECINSTR;t.data.resize(t.size);
 for(uint32_t o=0;o<t.size;o+=4)put(t,o,0x60000000u);
 put(t,0x00,0x28030002u); // cmplwi r3,2
 put(t,0x04,0x4181005Cu); // bgt +60 default
 put(t,0x08,0x48000010u); // skip fallback to +18
 put(t,0x0C,0x38600001u); // bounded fallback index 1
 put(t,0x10,0x90610000u); // ordinary store, index preserved
 put(t,0x14,0x81210000u); // independent load r9
 put(t,0x18,0x3C000200u); // separate base in r0
 put(t,0x1C,0x5463103Au); // slwi r3,r3,2
 put(t,0x20,0x30000030u); // addic r0,r0,table low
 put(t,0x24,0x7C030214u); // add r0,r3,r0
 put(t,0x28,0x7C0903A6u);put(t,0x2C,0x4E800420u); // mtctr/bctr
 put(t,0x30,0x48000010u);put(t,0x34,0x48000014u);put(t,0x38,0x48000018u);
 for(uint32_t o:{0x40u,0x48u,0x50u,0x60u})put(t,o,0x4E800020u);
 m.sections.push_back(t);return m;
}
static auto audit(const rpx::RpxModule& m){analysis::CFGBuilder cfg(m);return cfg.audit_inline_branch(base+0x2C);}
int main(){
 auto valid=fixture();auto accepted=audit(valid);require(accepted.targets.size()==3,"bounded merge table must resolve");require(accepted.targets.front()==base+0x30 && accepted.targets.back()==base+0x38,"exact table entries");
 analysis::CFGBuilder builder(valid);auto cfg=builder.build(base);require(cfg.has_value() && cfg->indirect_jump_targets.contains(base+0x2C) && cfg->indirect_jump_targets.at(base+0x2C).size()==3,"table targets must reach the function CFG");
 auto bad=fixture();put(bad.sections[0],0x0C,0x38600003u);require(audit(bad).targets.empty(),"fallback above upper bound rejected");
 bad=fixture();put(bad.sections[0],0x14,0x80610000u);require(audit(bad).targets.empty(),"load clobbering selector rejected");
 bad=fixture();put(bad.sections[0],0x70,0x4BFFFFA0u);require(audit(bad).targets.empty(),"branch into fallback interior rejected");
 bad=fixture();put(bad.sections[0],0x70,0x4BFFFFA8u);require(audit(bad).targets.empty(),"external branch bypassing guard rejected");
 bad=fixture();put(bad.sections[0],0x04,0x4182005Cu);require(audit(bad).targets.empty(),"wrong guard condition rejected");
 bad=fixture();put(bad.sections[0],0x34,0x60000000u);require(audit(bad).targets.empty(),"non-branch table slot rejected");
 std::puts("guarded inline-table acceptance/rejection contracts passed");
}
