// A small inline hook for 32-bit x86, for the one function a vtable patch cannot hold on to: it puts a jump to the
// hook over the function's first instructions and builds a trampoline that runs them and jumps back. It only moves
// plain prologue instructions (pushes, register moves, stack adjustments, loads and stores, nothing relative to the
// instruction pointer) and leaves any function that starts otherwise alone.
#pragma once

namespace detour {

// Returns the trampoline (call it to run the original function), or nullptr if the function was left alone.
void* install(void* target, void* hook);

}  // namespace detour
