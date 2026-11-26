#pragma once

#include <cstdint>

struct CPUState;

namespace rbrew_mod {

// Installs a native 5-byte x86-64 trampoline detour on `target` redirecting to `detour`.
// On success, writes the callable relocated prologue+return trampoline to `*out_trampoline`
// and an uninstall handle to `*out_id` (> 0).
bool install_native_trampoline(void* target, void* detour, void** out_trampoline, uint64_t* out_id);

// Restores the original prologue bytes of the trampoline identified by `id`.
bool remove_native_trampoline(uint64_t id);

// Generates an executable 2-arg thunk `(CPUState* cpu, uint8_t* mem)` forwarding to
// `void (*dispatcher)(CPUState* cpu, uint8_t* mem, uint32_t guest_addr, void* orig_trampoline)`.
void* create_guest_body_bridge(uint32_t guest_addr, void* orig_trampoline, void* dispatcher);

// Generates an executable 1-arg thunk `(CPUState* cpu)` forwarding to
// `void (*dispatcher)(CPUState* cpu, uint32_t guest_addr, void* prev_host_fn)`.
void* create_dispatch_bridge(uint32_t guest_addr, void* prev_host_fn, void* dispatcher);

// Generates an executable 1-arg thunk `(CPUState* cpu)` forwarding to
// `void (*orig_dispatcher)(CPUState* cpu, uintptr_t cookie)`.
void* create_guest_orig_stub(uintptr_t cookie, void* orig_dispatcher);

// Allocates a mutable 8-byte-aligned indirect jump stub (`jmp [rip+0]; .quad target`)
// and updates its target pointer in place without altering page execute permissions.
void* create_jump_stub(void* initial_target);
bool patch_jump_stub_target(void* stub, void* new_target);

} // namespace rbrew_mod
