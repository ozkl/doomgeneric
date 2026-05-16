#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Include Windows.h BEFORE wah.h — wah.h internally defines NOGDI which
// strips GDI types like BITMAPINFO. By including first, we get full GDI.
#include <Windows.h>

#define WAH_IMPLEMENTATION
#include "wah.h"

// ============================================================================
// Configuration
// ============================================================================

#define DOOM_RESX 640
#define DOOM_RESY 400
#define KEYQUEUE_SIZE 16

// ============================================================================
// Global state
// ============================================================================

// WAH execution context (needed by host functions to access WASM memory)
static wah_exec_context_t g_ctx;

// Win32
static HWND g_hwnd = NULL;
static HDC g_hdc = NULL;
static BITMAPINFO g_bmi = { { sizeof(BITMAPINFOHEADER), DOOM_RESX, -DOOM_RESY, 1, 32 } };

// Key queue
static unsigned short g_key_queue[KEYQUEUE_SIZE];
static unsigned int g_key_write = 0;
static unsigned int g_key_read = 0;

// File descriptor table
#define MAX_FDS 32
static FILE *g_fd_table[MAX_FDS];

static void fd_table_init(void) {
    memset(g_fd_table, 0, sizeof(g_fd_table));
    g_fd_table[0] = stdin;
    g_fd_table[1] = stdout;
    g_fd_table[2] = stderr;
}

static int fd_alloc(FILE *f) {
    for (int i = 3; i < MAX_FDS; i++) {
        if (!g_fd_table[i]) {
            g_fd_table[i] = f;
            return i;
        }
    }
    return -1;
}

// ============================================================================
// Helpers
// ============================================================================

// Read a file into a malloc'd buffer
static uint8_t *slurp(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open: %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(size);
    if (buf && fread(buf, 1, size, f) != (size_t)size) { free(buf); buf = NULL; }
    fclose(f);
    if (out_size) *out_size = (size_t)size;
    return buf;
}

// Get pointer into WASM linear memory
static uint8_t *wasm_mem(void) {
    return g_ctx.memory_base;
}

// ============================================================================
// Win32 Window
// ============================================================================

static unsigned char convertToDoomKey(unsigned char key) {
    switch (key) {
    case VK_RETURN:  return 13;   // KEY_ENTER
    case VK_ESCAPE:  return 27;   // KEY_ESCAPE
    case VK_LEFT:    return 0xac; // KEY_LEFTARROW
    case VK_RIGHT:   return 0xae; // KEY_RIGHTARROW
    case VK_UP:      return 0xad; // KEY_UPARROW
    case VK_DOWN:    return 0xaf; // KEY_DOWNARROW
    case VK_CONTROL: return 0xa3; // KEY_FIRE
    case VK_SPACE:   return 0xa2; // KEY_USE
    case VK_SHIFT:   return 0xb6; // KEY_RSHIFT
    case VK_MENU:    return 0xb4; // KEY_LALT (Alt key)
    default:         return tolower(key);
    }
}

static void addKeyToQueue(int pressed, unsigned char keyCode) {
    unsigned char key = convertToDoomKey(keyCode);
    g_key_queue[g_key_write] = (unsigned short)((pressed << 8) | key);
    g_key_write = (g_key_write + 1) % KEYQUEUE_SIZE;
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CLOSE:    DestroyWindow(hwnd); break;
    case WM_DESTROY:  PostQuitMessage(0); ExitProcess(0); break;
    case WM_KEYDOWN:  addKeyToQueue(1, (unsigned char)wParam); break;
    case WM_KEYUP:    addKeyToQueue(0, (unsigned char)wParam); break;
    default:          return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return 0;
}

static void createWindow(void) {
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc = wndProc;
    wc.lpszClassName = "DoomWAH";
    RegisterClassExA(&wc);

    RECT rect = {0, 0, DOOM_RESX, DOOM_RESY};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    g_hwnd = CreateWindowExA(0, "DoomWAH", "DOOM (WAH)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        NULL, NULL, NULL, NULL);
    g_hdc = GetDC(g_hwnd);
    ShowWindow(g_hwnd, SW_SHOW);
}

// ============================================================================
// Host functions: DG platform
// ============================================================================

static void hf_host_init(wah_call_context_t *ctx, void *ud) {
    (void)ctx; (void)ud;
    createWindow();
    printf("[host] Window created (%dx%d)\n", DOOM_RESX, DOOM_RESY);
}

static void hf_host_draw_frame(wah_call_context_t *ctx, void *ud) {
    (void)ud;
    uint32_t buf_ptr = wah_param_i32(ctx, 0);

    // Process Windows messages
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    // Blit from WASM memory to window
    uint8_t *pixels = wasm_mem() + buf_ptr;
    StretchDIBits(g_hdc, 0, 0, DOOM_RESX, DOOM_RESY,
                  0, 0, DOOM_RESX, DOOM_RESY,
                  pixels, &g_bmi, 0, SRCCOPY);
    SwapBuffers(g_hdc);
}

static void hf_host_sleep_ms(wah_call_context_t *ctx, void *ud) {
    (void)ud;
    uint32_t ms = wah_param_i32(ctx, 0);
    Sleep(ms);
}

static void hf_host_get_ticks_ms(wah_call_context_t *ctx, void *ud) {
    (void)ud;
    wah_return_i32(ctx, (int32_t)GetTickCount());
}

static void hf_host_get_key(wah_call_context_t *ctx, void *ud) {
    (void)ud;
    if (g_key_read == g_key_write) {
        wah_return_i32(ctx, 0);
    } else {
        unsigned short keyData = g_key_queue[g_key_read];
        g_key_read = (g_key_read + 1) % KEYQUEUE_SIZE;
        wah_return_i32(ctx, (int32_t)keyData);
    }
}

static void hf_host_set_window_title(wah_call_context_t *ctx, void *ud) {
    (void)ud;
    uint32_t title_ptr = wah_param_i32(ctx, 0);
    uint32_t len = wah_param_i32(ctx, 1);
    char buf[256];
    if (len > 255) len = 255;
    memcpy(buf, wasm_mem() + title_ptr, len);
    buf[len] = '\0';
    if (g_hwnd) SetWindowTextA(g_hwnd, buf);
}

// ============================================================================
// Host functions: File I/O
// ============================================================================

// host_fopen(path_ptr, mode_ptr) -> fd or -1
static void hf_host_fopen(wah_call_context_t *ctx, void *ud) {
    (void)ud;
    uint32_t path_ptr = wah_param_i32(ctx, 0);
    uint32_t mode_ptr = wah_param_i32(ctx, 1);

    uint8_t *mem = wasm_mem();
    const char *path = (const char *)(mem + path_ptr);
    const char *mode = (const char *)(mem + mode_ptr);
    printf("[host] fopen(\"%s\", \"%s\")\n", path, mode);

    FILE *f = fopen(path, mode);
    if (!f) {
        wah_return_i32(ctx, -1);
        return;
    }

    int fd = fd_alloc(f);
    if (fd < 0) {
        fclose(f);
        wah_return_i32(ctx, -1);
        return;
    }

    printf("[host] fopen: fd=%d\n", fd);
    wah_return_i32(ctx, fd);
}

// host_fclose(fd) -> 0 on success
static void hf_host_fclose(wah_call_context_t *ctx, void *ud) {
    (void)ud;
    int32_t fd = wah_param_i32(ctx, 0);
    if (fd < 3 || fd >= MAX_FDS || !g_fd_table[fd]) {
        wah_return_i32(ctx, -1);
        return;
    }
    fclose(g_fd_table[fd]);
    g_fd_table[fd] = NULL;
    wah_return_i32(ctx, 0);
}

// host_fread(fd, buf_ptr, size) -> bytes_read
static void hf_host_fread(wah_call_context_t *ctx, void *ud) {
    (void)ud;
    int32_t fd = wah_param_i32(ctx, 0);
    uint32_t buf_ptr = wah_param_i32(ctx, 1);
    int32_t size = wah_param_i32(ctx, 2);

    FILE *f = (fd >= 0 && fd < MAX_FDS) ? g_fd_table[fd] : NULL;
    if (!f) { wah_return_i32(ctx, -1); return; }

    uint8_t *dest = wasm_mem() + buf_ptr;
    size_t rd = fread(dest, 1, size, f);
    wah_return_i32(ctx, (int32_t)rd);
}

// host_fwrite(fd, buf_ptr, size) -> bytes_written
static void hf_host_fwrite(wah_call_context_t *ctx, void *ud) {
    (void)ud;
    int32_t fd = wah_param_i32(ctx, 0);
    uint32_t buf_ptr = wah_param_i32(ctx, 1);
    int32_t size = wah_param_i32(ctx, 2);

    FILE *f = (fd >= 0 && fd < MAX_FDS) ? g_fd_table[fd] : NULL;
    if (!f) { wah_return_i32(ctx, -1); return; }

    uint8_t *src = wasm_mem() + buf_ptr;
    size_t wr = fwrite(src, 1, size, f);
    fflush(f);
    wah_return_i32(ctx, (int32_t)wr);
}

// host_fseek(fd, offset, whence) -> 0 on success
static void hf_host_fseek(wah_call_context_t *ctx, void *ud) {
    (void)ud;
    int32_t fd = wah_param_i32(ctx, 0);
    int32_t offset = wah_param_i32(ctx, 1);
    int32_t whence = wah_param_i32(ctx, 2);

    FILE *f = (fd >= 0 && fd < MAX_FDS) ? g_fd_table[fd] : NULL;
    if (!f) { wah_return_i32(ctx, -1); return; }

    int c_whence = SEEK_SET;
    if (whence == 1) c_whence = SEEK_CUR;
    else if (whence == 2) c_whence = SEEK_END;

    int result = fseek(f, (long)offset, c_whence);
    wah_return_i32(ctx, result);
}

// host_ftell(fd) -> position
static void hf_host_ftell(wah_call_context_t *ctx, void *ud) {
    (void)ud;
    int32_t fd = wah_param_i32(ctx, 0);

    FILE *f = (fd >= 0 && fd < MAX_FDS) ? g_fd_table[fd] : NULL;
    if (!f) { wah_return_i32(ctx, -1); return; }

    wah_return_i32(ctx, (int32_t)ftell(f));
}

// ============================================================================
// Host functions: Console output + process control
// ============================================================================

// host_puts(str_ptr, len) -> void
static void hf_host_puts(wah_call_context_t *ctx, void *ud) {
    (void)ud;
    uint32_t str_ptr = wah_param_i32(ctx, 0);
    int32_t len = wah_param_i32(ctx, 1);

    const char *str = (const char *)(wasm_mem() + str_ptr);
    fwrite(str, 1, len, stdout);
    fflush(stdout);
}

// host_exit(code) -> noreturn
static void hf_host_exit(wah_call_context_t *ctx, void *ud) {
    (void)ud;
    int32_t code = wah_param_i32(ctx, 0);
    printf("[host] exit(%d)\n", code);
    exit(code);
}

// ============================================================================
// Stubs for musl libc internal imports (not used by our doom code)
// ============================================================================

static void hf_stub_i32(wah_call_context_t *ctx, void *ud) {
    (void)ud;
    (void)ctx;
    wah_return_i32(ctx, -1);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char *argv[]) {
    const char *wasm_path = "doomgeneric/doom.wasm";
    if (argc > 1) wasm_path = argv[1];

    fd_table_init();

    // 1. Load WASM binary
    size_t wasm_size;
    uint8_t *wasm_buf = slurp(wasm_path, &wasm_size);
    if (!wasm_buf) {
        fprintf(stderr, "Failed to load %s\n", wasm_path);
        return 1;
    }
    printf("Loaded %s (%zu bytes)\n", wasm_path, wasm_size);

    wah_error_t err;

    // 2. Parse guest module
    wah_module_t doom_mod = {0};
    if ((err = wah_parse_module(&doom_mod, wasm_buf, wasm_size, NULL))) {
        fprintf(stderr, "wah_parse_module: %s\n", wah_strerror(err));
        return 1;
    }
    free(wasm_buf);
    printf("Module parsed OK\n");

    // 3. Create "env" host module — ALL host functions go here
    wah_module_t env_mod = {0};
    if ((err = wah_new_module(&env_mod, NULL))) goto fail;

    // DG platform functions
    if ((err = wah_export_func(&env_mod, "host_init",             "()",                 hf_host_init,        NULL, NULL))) goto fail;
    if ((err = wah_export_func(&env_mod, "host_draw_frame",       "(i32, i32, i32)",    hf_host_draw_frame,  NULL, NULL))) goto fail;
    if ((err = wah_export_func(&env_mod, "host_sleep_ms",         "(i32)",              hf_host_sleep_ms,    NULL, NULL))) goto fail;
    if ((err = wah_export_func(&env_mod, "host_get_ticks_ms",     "() -> i32",          hf_host_get_ticks_ms,NULL, NULL))) goto fail;
    if ((err = wah_export_func(&env_mod, "host_get_key",          "() -> i32",          hf_host_get_key,     NULL, NULL))) goto fail;
    if ((err = wah_export_func(&env_mod, "host_set_window_title", "(i32, i32)",         hf_host_set_window_title, NULL, NULL))) goto fail;

    // File I/O
    if ((err = wah_export_func(&env_mod, "host_fopen",            "(i32, i32) -> i32",        hf_host_fopen,  NULL, NULL))) goto fail;
    if ((err = wah_export_func(&env_mod, "host_fclose",           "(i32) -> i32",             hf_host_fclose, NULL, NULL))) goto fail;
    if ((err = wah_export_func(&env_mod, "host_fread",            "(i32, i32, i32) -> i32",   hf_host_fread,  NULL, NULL))) goto fail;
    if ((err = wah_export_func(&env_mod, "host_fwrite",           "(i32, i32, i32) -> i32",   hf_host_fwrite, NULL, NULL))) goto fail;
    if ((err = wah_export_func(&env_mod, "host_fseek",            "(i32, i32, i32) -> i32",   hf_host_fseek,  NULL, NULL))) goto fail;
    if ((err = wah_export_func(&env_mod, "host_ftell",            "(i32) -> i32",             hf_host_ftell,  NULL, NULL))) goto fail;

    // Console + process
    if ((err = wah_export_func(&env_mod, "host_puts",             "(i32, i32)",               hf_host_puts,   NULL, NULL))) goto fail;
    if ((err = wah_export_func(&env_mod, "host_exit",             "(i32)",                    hf_host_exit,   NULL, NULL))) goto fail;

    // Emscripten syscall stubs (musl libc internals still import these)
    if ((err = wah_export_func(&env_mod, "__syscall_unlinkat",    "(i32, i32, i32) -> i32",   hf_stub_i32,    NULL, NULL))) goto fail;
    if ((err = wah_export_func(&env_mod, "__syscall_rmdir",       "(i32) -> i32",             hf_stub_i32,    NULL, NULL))) goto fail;
    if ((err = wah_export_func(&env_mod, "__syscall_renameat",    "(i32, i32, i32, i32) -> i32", hf_stub_i32, NULL, NULL))) goto fail;
    if ((err = wah_export_func(&env_mod, "_emscripten_system",    "(i32) -> i32",             hf_stub_i32,    NULL, NULL))) goto fail;

    // 4. Create "wasi_snapshot_preview1" module (musl libc still uses these internally)
    wah_module_t wasi_mod = {0};
    if ((err = wah_new_module(&wasi_mod, NULL))) goto fail;
    if ((err = wah_export_func(&wasi_mod, "fd_close", "(i32) -> i32",                hf_stub_i32, NULL, NULL))) goto fail;
    if ((err = wah_export_func(&wasi_mod, "fd_write", "(i32, i32, i32, i32) -> i32", hf_stub_i32, NULL, NULL))) goto fail;
    if ((err = wah_export_func(&wasi_mod, "fd_seek",  "(i32, i64, i32, i32) -> i32", hf_stub_i32, NULL, NULL))) goto fail;

    // 5. Create execution context + link
    memset(&g_ctx, 0, sizeof(g_ctx));
    if ((err = wah_new_exec_context(&g_ctx, &doom_mod, NULL))) goto fail;
    if ((err = wah_link_module(&g_ctx, "env", &env_mod))) goto fail;
    if ((err = wah_link_module(&g_ctx, "wasi_snapshot_preview1", &wasi_mod))) goto fail;
    printf("Modules linked OK\n");

    // 5. Call _initialize (emscripten runtime init)
    printf("Calling _initialize...\n");
    if ((err = wah_call_by_name(&g_ctx, "_initialize", NULL, 0, NULL))) {
        fprintf(stderr, "_initialize failed: %s\n", wah_strerror(err));
        goto fail;
    }

    // 6. Call doom_init (loads WAD, sets up game)
    printf("Calling doom_init...\n");
    if ((err = wah_call_by_name(&g_ctx, "doom_init", NULL, 0, NULL))) {
        fprintf(stderr, "doom_init failed: %s\n", wah_strerror(err));
        goto fail;
    }
    printf("Doom initialized!\n");

    // 7. Main loop: call doom_tick every frame
    for (;;) {
        if ((err = wah_call_by_name(&g_ctx, "doom_tick", NULL, 0, NULL))) {
            fprintf(stderr, "doom_tick failed: %s\n", wah_strerror(err));
            break;
        }
    }

    wah_free_exec_context(&g_ctx);
    wah_free_module(&wasi_mod);
    wah_free_module(&env_mod);
    wah_free_module(&doom_mod);
    return 0;

fail:
    fprintf(stderr, "error: %s\n", wah_strerror(err));
    return 1;
}
