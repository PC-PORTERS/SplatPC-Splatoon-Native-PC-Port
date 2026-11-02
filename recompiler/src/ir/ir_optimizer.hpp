#pragma once
#include "ir_function.hpp"

namespace rebrewu::ir {

// Final optimization phase, run after CFG/IR construction and before code
// generation. Three passes over each function:
//   1. simplify — constant folding, copy propagation and CSE on pure temps
//   2. fuse_compare_branches — cmp + CR-bit extract + test + branch chains
//      become direct compare-and-branch opcodes (host conditionals, no
//      materialised CR field)
//   3. simplify again — drops the fused-away extraction chain
void optimize_function(IRFunction& func);

} // namespace rebrewu::ir