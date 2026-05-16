# AGENTS.md — Rules for AI assistants working on this project

## Do NOT rely on libc internals

When working with the WASM/emscripten toolchain:

- **Use only the C standard library's public interface** (e.g., `fopen`, `printf`, `exit`).
- **Do NOT reference or override internal implementation details** of musl, emscripten, or any libc.
  - No `__wasi_fd_*` functions
  - No `__syscall_*` functions
  - No internal struct layouts (e.g., `struct _IO_FILE`)
  - No assumptions about how libc functions are implemented internally
- **libc internals can change** between versions. Any code that depends on them is fragile and will break.
- If musl/emscripten generates WASM imports for internal functions (e.g., `wasi_snapshot_preview1.fd_write`, `env.__syscall_unlinkat`), provide stub implementations **in the host runtime**, not by overriding them in guest C code.
