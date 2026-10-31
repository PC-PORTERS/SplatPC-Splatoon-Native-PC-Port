#include "analysis/cfg_builder.hpp"
#include "core/rpx/rpx_loader.hpp"
#include "diagnostics/diagnostics.hpp"
#include <cstdio>
#include <string>
int main(int argc,char** argv){
 if(argc!=3){std::fprintf(stderr,"usage: inspect_inline_branch owned.rpx branch_pc_hex\n");return 2;}
 rebrewu::diagnostics::DiagEngine diagnostics;
 rebrewu::rpx::RpxLoader loader(diagnostics);auto module=loader.load(argv[1]);if(!module)return 2;
 rebrewu::analysis::CFGBuilder builder(*module);const auto result=builder.audit_inline_branch(uint32_t(std::stoul(argv[2],nullptr,16)));
 std::printf("proof=%s targets=%zu\n",result.reason.c_str(),result.targets.size());
 for(auto target:result.targets)std::printf("%08X\n",target);
 return result.targets.empty()?1:0;
}
