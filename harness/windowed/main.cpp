#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <retropark/retropark.h>
#include <string>

static rp_runtime* g_rt = nullptr;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = L"RetroParkHarness";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"RetroPark Slice A",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
        nullptr, nullptr, hInst, nullptr);

    g_rt = rp_runtime_create(RP_GFX_D3D11, hwnd);
    rp_runtime_resize(g_rt, 640, 480);
    // Core dir is passed at build time; fall back to a relative path.
#ifndef RP_HARNESS_CORE_DIR
#define RP_HARNESS_CORE_DIR "cores/refcore_present"
#endif
    rp_runtime_load_core(g_rt, RP_HARNESS_CORE_DIR);

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
