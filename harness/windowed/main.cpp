#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <retropark/retropark.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

static rp_runtime* g_rt = nullptr;

// F5/F7 savestate + a held-key rewind, for the human demo (Slice F Task 5). These are
// no-ops (the runtime C API just returns RP_ERR_UNSUPPORTED / does nothing useful) for a
// presenting core with no serialize support, so wiring them unconditionally is harmless;
// they only do something visible with --driven or --content.
static std::vector<uint8_t> g_save_buf;
static bool g_has_saved = false;
static bool g_rewind_enabled = false;
// Rewind key: hold LEFT ARROW to step backward one frame per tick; release to resume forward.
static const int kRewindVK = VK_LEFT;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (m == WM_KEYDOWN && g_rt && ((static_cast<uint32_t>(l) >> 30) & 1u) == 0u) {   // ignore auto-repeat
        if (w == VK_F5) {
            size_t sz = rp_runtime_serialize_size(g_rt);
            if (sz == 0) {
                printf("[harness] save: unsupported (no serialize-capable core loaded)\n");
            } else {
                g_save_buf.assign(sz, 0);
                rp_result r = rp_runtime_save_state(g_rt, g_save_buf.data(), g_save_buf.size());
                if (r == RP_OK) {
                    g_has_saved = true;
                    printf("[harness] saved state, %zu bytes\n", sz);
                } else {
                    g_has_saved = false;
                    printf("[harness] save failed (result=%d)\n", (int)r);
                }
            }
            fflush(stdout);
        } else if (w == VK_F7) {
            if (!g_has_saved) {
                printf("[harness] load: no saved state\n");
            } else {
                rp_result r = rp_runtime_load_state(g_rt, g_save_buf.data(), g_save_buf.size());
                printf("[harness] load state: %s\n", r == RP_OK ? "ok" : "failed");
            }
            fflush(stdout);
        }
    }
    return DefWindowProc(h, m, w, l);
}

// Core dirs are baked in at build time; fall back to relative paths.
#ifndef RP_HARNESS_CORE_DIR
#define RP_HARNESS_CORE_DIR "cores/refcore_present"
#endif
#ifndef RP_HARNESS_CORE_DIR_VK
#define RP_HARNESS_CORE_DIR_VK "cores/refcore_present_vk"
#endif
#ifndef RP_HARNESS_DRIVEN_CORE_DIR
#define RP_HARNESS_DRIVEN_CORE_DIR "cores/refcore_driven"
#endif
#ifndef RP_HARNESS_SHIM_DIR
#define RP_HARNESS_SHIM_DIR "cores/libretro_shim"
#endif

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), len, nullptr, nullptr);
    return s;
}

// A WS_OVERLAPPEDWINDOW/WinMain app has no console by default, so printf from the F5/F7/
// rewind handlers above would otherwise vanish silently. Attach the launching console when
// there is one (running from a shell), else allocate a fresh one, and route stdout to it.
static void attach_console_output() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS) && !AllocConsole()) return;
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    attach_console_output();
    // Parse `--api vulkan|d3d11` (default d3d11), `--driven`, and `--content <rom>`
    // (the next arg is the ROM path) from the process command line.
    bool use_vulkan = false;
    bool use_driven = false;
    std::string content_path;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 0; i < argc; i++) {
                std::wstring a = argv[i];
                if (a == L"--api" && i + 1 < argc) {
                    if (std::wstring(argv[i + 1]) == L"vulkan") use_vulkan = true;
                } else if (a == L"--api=vulkan") {
                    use_vulkan = true;
                } else if (a == L"--driven") {
                    use_driven = true;
                } else if (a == L"--content" && i + 1 < argc) {
                    content_path = narrow(argv[i + 1]);
                }
            }
            LocalFree(argv);
        }
    }
    const bool use_content = !content_path.empty();
    const rp_graphics_api api = use_vulkan ? RP_GFX_VULKAN : RP_GFX_D3D11;
    const char* core_dir = use_content
        ? RP_HARNESS_SHIM_DIR
        : (use_driven
            ? RP_HARNESS_DRIVEN_CORE_DIR
            : (use_vulkan ? RP_HARNESS_CORE_DIR_VK : RP_HARNESS_CORE_DIR));

    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = L"RetroParkHarness";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"RetroPark Slice A",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
        nullptr, nullptr, hInst, nullptr);

    g_rt = rp_runtime_create(api, hwnd);
    if (use_content) {
        rp_runtime_resize(g_rt, 256, 240);   // NES resolution
        rp_runtime_load_core(g_rt, core_dir);
        rp_runtime_load_content(g_rt, content_path.c_str());
    } else {
        rp_runtime_resize(g_rt, 640, 480);
        rp_runtime_load_core(g_rt, core_dir);
    }

    // Rewind only makes sense against a serialize-capable driven/content core; on a plain
    // presenting core this returns RP_ERR_UNSUPPORTED and g_rewind_enabled just stays false,
    // making the held rewind key a harmless no-op below.
    if (use_content || use_driven) {
        g_rewind_enabled = (rp_runtime_set_rewind(g_rt, 1, 600) == RP_OK);
        if (!g_rewind_enabled) printf("[harness] rewind unsupported for this core\n");
    }
    printf("[harness] keys: F5 = save state, F7 = load state, hold LEFT ARROW = rewind\n");
    fflush(stdout);

    MSG msg{};
    bool running = true;
    while (running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        const bool rewinding = g_rewind_enabled && (GetAsyncKeyState(kRewindVK) & 0x8000) != 0;
        if (rewinding) {
            // Step one frame into the past and show it. RP_ERR_NOT_FOUND means the ring is
            // empty (no more history) -- stop going back and hold on the current frame
            // (skip the present this tick) rather than lurching forward again while the key
            // is still held.
            if (rp_runtime_rewind(g_rt) == RP_OK) {
                rp_runtime_present(g_rt, nullptr);
            }
        } else {
            rp_runtime_present(g_rt, nullptr);   // normal forward present to the window
        }
    }
    rp_runtime_unload_core(g_rt);
    rp_runtime_destroy(g_rt);
    return 0;
}
