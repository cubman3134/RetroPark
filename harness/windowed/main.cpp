#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <retropark/retropark.h>
#include <string>

static rp_runtime* g_rt = nullptr;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
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

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
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

    MSG msg{};
    bool running = true;
    while (running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        rp_runtime_present(g_rt, nullptr);   // present to the window
    }
    rp_runtime_unload_core(g_rt);
    rp_runtime_destroy(g_rt);
    return 0;
}
