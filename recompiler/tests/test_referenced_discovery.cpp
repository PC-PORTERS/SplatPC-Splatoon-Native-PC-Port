#include "analysis/function_discovery.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
using namespace rebrewu;
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static void put(rpx::RpxSection& section,uint32_t offset,uint32_t value){for(unsigned i=0;i<4;++i)section.data[offset+i]=uint8_t(value>>(24-8*i));}
static bool contains(const std::vector<analysis::FunctionBoundary>& list,uint32_t target){return std::any_of(list.begin(),list.end(),[&](const auto& item){return item.start==target;});}
int main(){
 constexpr uint32_t base=0x02000000u;
 rpx::RpxModule module{};module.name="authored_fixture";module.entry_point=base;
 rpx::RpxSection text{};text.name=".text";text.address=base;text.size=0x200;text.flags=elf::SHF_ALLOC|elf::SHF_EXECINSTR;text.data.resize(text.size);
 for(uint32_t off=0;off<text.size;off+=4)put(text,off,0x60000000u);
 put(text,0,0x4E800020u); // authored BLR
 for(uint32_t off:{0x40u,0x80u,0xC0u,0x100u}){put(text,off,0x38600007u);put(text,off+4,0x4E800020u);}
 put(text,0x3C,0x4E800020u); // isolated referenced entry
 put(text,0x7C,0x38600000u); // fallthrough into interior data reference
 put(text,0xBC,0x4E800020u);put(text,0xFC,0x4E800020u);
 put(text,0xA0,0x41820020u); // conditional branch into +C0: reject as interior
 module.sections.push_back(text);
 for(uint32_t off:{0x40u,0x80u,0xC0u}){
  rpx::RpxReloc relocation{};relocation.offset=base+0x20;relocation.type=elf::R_PPC_ADDR32;relocation.sym_index=99;relocation.sym_value=base+off;module.relocations.push_back(relocation);
 }
 diagnostics::DiagEngine diagnostics;linker::Linker linker(diagnostics);
 analysis::FunctionDiscovery::Config config;config.follow_calls=false;config.analyze_symbol_table=false;config.analyze_exports=false;
 analysis::FunctionDiscovery unchanged(module,linker,config);auto original=unchanged.discover();
 require(!contains(original,base+0x40),"default discovery must remain unchanged");
 config.discover_referenced_entries=true;
 analysis::FunctionDiscovery expanded(module,linker,config);auto result=expanded.discover();
 require(contains(result,base+0x40),"reference plus separator must discover the isolated entry");
 require(!contains(result,base+0x80),"fallthrough interior must not become a function");
 require(!contains(result,base+0xC0),"conditional branch target must not become a function");
 require(!contains(result,base+0x100),"unreferenced bytes must not become a function");
 require(expanded.stats().from_corroborated_entries==1,"exactly one new entry has independent proofs");
 std::puts("referenced-boundary acceptance/rejection contracts passed");
}
