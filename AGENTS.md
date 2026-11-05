# Agents.md -> READ THIS IF YOU ARE AN AI AGENT

- Always follow Clang-format
- Clean, straightforward code : prioritize clean code over micro-optimizations that the compiler already handles.
- Don't over-comment the code: just enough to understand how it works.
- Maintain modular code.
- Writing tests when making major changes.
- Use the appropriate logging module to display information in the console: NO C++ `std::cout` + NO C-style output.
- All game-related modification should be made in the manifest (simple Patch), or in the Hooks folder
