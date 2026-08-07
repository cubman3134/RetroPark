#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    // Parse `--api vulkan|d3d11` (default d3d11) from the process command line.
    bool use_vulkan = false;
    {
        std::wstring cmd = GetCommandLineW() ? GetCommandLineW() : L"";
        if (cmd.find(L"--api vulkan") != std::wstring::npos ||
            cmd.find(L"--api=vulkan") != std::wstring::npos) {
            use_vulkan = true;
        }
    }
    const rp_graphics_api api = use_vulkan ? RP_GFX_VULKAN : RP_GFX_D3D11;
    const char* core_dir = use_vulkan ? RP_HARNESS_CORE_DIR_VK : RP_HARNESS_CORE_DIR;

    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = L"RetroParkHarness";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"RetroPark Slice A",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
        nullptr, nullptr, hInst, nullptr);

    g_rt = rp_runtime_create(api, hwnd);
    rp_runtime_resize(g_rt, 640, 480);
    rp_runtime_load_core(g_rt, core_dir);

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
