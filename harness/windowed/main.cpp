#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <retropark/retropark.h>
#include "net/NetSession.h"
#include "net/RollbackSession.h"
#include "net/TcpTransport.h"
#include "net/Crc32.h"
#include "runtime/Runtime.h"
#include "xinput_map.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
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
        } else if (w == 'P') {                    // toggle pause
            rp_runtime_status st{}; rp_runtime_get_status(g_rt, &st);
            if (st.paused) rp_runtime_resume(g_rt); else rp_runtime_pause(g_rt);
        } else if (w == VK_F8) {                  // reset
            rp_runtime_reset(g_rt);
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

// Netplay input capture: the harness normally wires no input at all (the single-player
// cores just free-run), so netplay is the first path that needs a live local snapshot per
// frame. Scan the whole VK range and mirror "currently down" into a rp_input_state -- simple
// and complete (no per-game button mapping to keep in sync between two machines).
static rp_input_state read_local_input() {
    rp_input_state s{};
    for (int vk = 0; vk < 256; ++vk) {
        if (GetAsyncKeyState(vk) & 0x8000) s.keys[vk] = 1;
    }
    static int xi_probe = 0;
    static bool xi_connected = true;
    if (xi_connected || (++xi_probe % 120) == 0) {   // when absent, re-probe only every ~120 frames
        XINPUT_STATE xi{};
        if (XInputGetState(0, &xi) == ERROR_SUCCESS) {
            xinput_to_pad(xi.Gamepad, s);   // merge gamepad into the same rp_input_state
            xi_connected = true;
        } else {
            xi_connected = false;
        }
    }
    return s;
}

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
    std::string custom_core_dir;
    // Netplay (manual 2-machine demo): --netplay-host <port> or --netplay-join <ip:port>.
    bool netplay_host_flag = false;
    bool netplay_join_flag = false;
    bool rollback_flag = false;
    std::string netplay_ip;
    uint16_t netplay_port = 0;
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
                } else if (a == L"--core" && i + 1 < argc) {
                    custom_core_dir = narrow(argv[i + 1]);
                } else if (a == L"--netplay-host" && i + 1 < argc) {
                    netplay_host_flag = true;
                    netplay_port = static_cast<uint16_t>(std::atoi(narrow(argv[i + 1]).c_str()));
                } else if (a == L"--netplay-join" && i + 1 < argc) {
                    netplay_join_flag = true;
                    std::string spec = narrow(argv[i + 1]);
                    size_t colon = spec.find(':');
                    if (colon != std::string::npos) {
                        netplay_ip = spec.substr(0, colon);
                        netplay_port = static_cast<uint16_t>(std::atoi(spec.substr(colon + 1).c_str()));
                    }
                } else if (a == L"--rollback") {
                    rollback_flag = true;
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
    // Identifies the loaded core to the netplay handshake; both machines must load the same
    // core and pass the same id here, or start_host/start_join reject the mismatch.
    const char* core_id = use_content
        ? "fceumm"
        : (use_driven ? "refcore_driven" : (use_vulkan ? "refcore_present_vk" : "refcore_present"));

    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = L"RetroParkHarness";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"RetroPark Slice A",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
        nullptr, nullptr, hInst, nullptr);

    // Dolphin netplay (Slice O): the vehicle exposes its own built-in netplay via C exports on
    // dolphin_present.dll. These are resolved only on the --core path (and only if a netplay flag is set);
    // the refcore/driven/content lockstep+rollback netplay below is a completely separate mechanism.
    using np_start_fn  = int (*)();
    using np_status_fn = void (*)(int*, int*, int*, uint32_t*, uint32_t*);   // + frame-progress counter
    np_start_fn  dp_np_start  = nullptr;   // host calls this once the joiner connects (see message loop)
    np_status_fn dp_np_status = nullptr;
    bool dolphin_netplay_host = false;     // this machine hosts Dolphin netplay and still owes a start()

    g_rt = rp_runtime_create(api, hwnd);
    if (!custom_core_dir.empty()) {
        // Arbitrary Vulkan presenting core (e.g. dolphin_present). --content feeds it the ROM; F5/F7 then
        // save/load it via the already-generic key handler.
        rp_runtime_resize(g_rt, 640, 480);
        rp_runtime_load_core(g_rt, custom_core_dir.c_str());
        // Dolphin netplay wiring (--core path only). The Runtime already loaded dolphin_present.dll; a
        // LoadLibraryExA on the same path just returns a (refcounted) handle so we can GetProcAddress the
        // netplay exports. Both peers arm the identical ROM first (each builds the same SyncIdentifier),
        // then host()/join(); the host defers start() to the message loop, once the joiner has connected.
        if ((netplay_host_flag || netplay_join_flag) && use_content) {
            std::string dll = custom_core_dir + "\\dolphin_present.dll";
            HMODULE core = LoadLibraryExA(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            using np_arm_fn  = int (*)(const char*);
            using np_host_fn = int (*)(uint16_t);
            using np_join_fn = int (*)(const char*, uint16_t);
            np_arm_fn  np_arm  = core ? (np_arm_fn)GetProcAddress(core, "rp_dolphin_netplay_arm")   : nullptr;
            np_host_fn np_host = core ? (np_host_fn)GetProcAddress(core, "rp_dolphin_netplay_host")  : nullptr;
            np_join_fn np_join = core ? (np_join_fn)GetProcAddress(core, "rp_dolphin_netplay_join")  : nullptr;
            dp_np_start        = core ? (np_start_fn)GetProcAddress(core, "rp_dolphin_netplay_start") : nullptr;
            dp_np_status       = core ? (np_status_fn)GetProcAddress(core, "rp_dolphin_netplay_status") : nullptr;
            if (np_arm && np_host && np_join && dp_np_start && dp_np_status) {
                np_arm(content_path.c_str());
                if (netplay_host_flag) {
                    if (np_host(netplay_port) == 0) {
                        dolphin_netplay_host = true;
                        printf("[harness] dolphin netplay: hosting on port %u, waiting for a peer...\n", netplay_port);
                    } else {
                        printf("[harness] dolphin netplay: host(%u) failed\n", netplay_port);
                    }
                } else {
                    int jr = np_join(netplay_ip.c_str(), netplay_port);
                    printf("[harness] dolphin netplay: join %s:%u -> %s\n", netplay_ip.c_str(),
                           netplay_port, jr == 0 ? "connected" : "failed");
                }
                fflush(stdout);
            } else {
                printf("[harness] dolphin netplay: could not resolve rp_dolphin_netplay_* exports on %s\n",
                       dll.c_str());
                fflush(stdout);
            }
        }
        if (use_content) rp_runtime_load_content(g_rt, content_path.c_str());
    } else if (use_content) {
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

    // Netplay (manual 2-machine demo): --netplay-host <port> hosts and waits for a peer to
    // connect (blocks on accept up to its timeout -- expected when launched with no peer);
    // --netplay-join <ip:port> connects to a host already waiting. content_hash is the ROM's
    // crc32 (0 for a contentless core) so a mismatched ROM/core between the two machines is
    // rejected at the handshake rather than silently desyncing later.
    // --rollback (only meaningful alongside --netplay-host/--netplay-join) swaps the Slice G
    // lockstep NetSession for a predictive RollbackSession: locally simulates ahead of the
    // confirmed remote frame instead of blocking on it, and resimulates+rolls back when a
    // prediction turns out wrong.
    std::unique_ptr<rp::net::TcpTransport> netplay_transport;
    rp::net::NetSession netplay_session;
    rp::net::RollbackSession rb_session;
    bool netplay = false;
    uint64_t content_hash = 0;
    const uint32_t out_w = use_content ? 256 : 640;
    const uint32_t out_h = use_content ? 240 : 480;
    std::vector<uint8_t> rb_out(static_cast<size_t>(out_w) * out_h * 4);
    if (use_content) {
        std::ifstream rom_f(content_path, std::ios::binary);
        if (rom_f) {
            std::vector<uint8_t> rom_bytes((std::istreambuf_iterator<char>(rom_f)), std::istreambuf_iterator<char>());
            content_hash = rp::net::crc32(rom_bytes.data(), rom_bytes.size());
        }
    }
    // The refcore lockstep/rollback netplay path (TcpTransport + NetSession) is only for the driven/content
    // cores. On the --core (Dolphin) path the same flags drove the vehicle's own netplay above, so skip
    // this block entirely there (custom_core_dir non-empty) and let the present loop run unchanged.
    if (custom_core_dir.empty() && netplay_host_flag) {
        std::string err;
        rp::Runtime& rt_cpp = *reinterpret_cast<rp::Runtime*>(g_rt);
        if (rp::net::TcpTransport::host(netplay_port, netplay_transport, err, 30000) != RP_OK) {
            fprintf(stderr, "netplay host failed: %s\n", err.c_str());
            return 1;
        }
        if (rollback_flag) {
            if (rb_session.start_host(rt_cpp, *netplay_transport, /*max_pred=*/8, content_hash, core_id, err) != RP_OK) {
                fprintf(stderr, "rollback host handshake failed: %s\n", err.c_str());
                return 1;
            }
            printf("netplay: hosting (rollback) on port %u, you are Player 1 (port 0)\n", netplay_port);
        } else {
            if (netplay_session.start_host(rt_cpp, *netplay_transport, /*delay=*/2, content_hash, core_id, err) != RP_OK) {
                fprintf(stderr, "host handshake failed: %s\n", err.c_str());
                return 1;
            }
            printf("netplay: hosting on port %u, you are Player 1 (port 0)\n", netplay_port);
        }
        netplay = true;
        fflush(stdout);
    } else if (custom_core_dir.empty() && netplay_join_flag) {
        std::string err;
        rp::Runtime& rt_cpp = *reinterpret_cast<rp::Runtime*>(g_rt);
        if (rp::net::TcpTransport::join(netplay_ip.c_str(), netplay_port, netplay_transport, err) != RP_OK) {
            fprintf(stderr, "netplay join failed: %s\n", err.c_str());
            return 1;
        }
        if (rollback_flag) {
            if (rb_session.start_join(rt_cpp, *netplay_transport, content_hash, core_id, err) != RP_OK) {
                fprintf(stderr, "rollback join handshake failed: %s\n", err.c_str());
                return 1;
            }
            printf("netplay: joined %s:%u (rollback), you are Player 2 (port 1)\n", netplay_ip.c_str(), netplay_port);
        } else {
            if (netplay_session.start_join(rt_cpp, *netplay_transport, content_hash, core_id, err) != RP_OK) {
                fprintf(stderr, "join handshake failed: %s\n", err.c_str());
                return 1;
            }
            printf("netplay: joined %s:%u, you are Player 2 (port 1)\n", netplay_ip.c_str(), netplay_port);
        }
        netplay = true;
        fflush(stdout);
    }

    MSG msg{};
    bool running = true;
    while (running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        // Dolphin netplay host: once the joiner has connected (players>=2), start the game exactly once.
        // Both peers then receive Dolphin's StartGame and boot; the normal present branch below shows the
        // frames the vehicle produces. One-shot — clear the flag so start() is not re-issued every frame.
        if (dolphin_netplay_host && dp_np_status && dp_np_start) {
            int c = 0, s = 0, d = 0; uint32_t players = 0, frame = 0;
            dp_np_status(&c, &s, &d, &players, &frame);
            if (players >= 2) {
                int sr = dp_np_start();
                printf("[harness] dolphin netplay: peer connected (players=%u), start() -> %d\n", players, sr);
                fflush(stdout);
                dolphin_netplay_host = false;
            }
        }
        if (netplay && rollback_flag) {
            rp_input_state local = read_local_input();
            // tick() advances + renders into rb_out via rp_runtime_render, which composites to
            // this same windowed runtime's swapchain internally (same path rp_runtime_present
            // uses) -- so rb_out itself is only needed as the required out_rgba destination;
            // there is nothing further to blit here. Stalled is normal (waiting on the peer to
            // catch up within max_prediction) -- the frame was already re-rendered, so just
            // loop around; only Desync/Disconnected halt the demo.
            rp::net::RbStatus st = rb_session.tick(local, rb_out.data());
            if (st == rp::net::RbStatus::Desync) {
                printf("DESYNC at frame %llu -- halting rollback netplay\n", (unsigned long long)rb_session.frame());
                fflush(stdout);
                break;
            }
            if (st == rp::net::RbStatus::Disconnected) {
                printf("peer disconnected -- halting rollback netplay\n");
                fflush(stdout);
                break;
            }
        } else if (netplay) {
            rp_input_state local = read_local_input();
            // Sends local, waits for the remote frame, advances the core with both, and
            // presents -- rp_runtime_present() runs INSIDE tick() against this same windowed
            // runtime (same swapchain as the single-player path below), so there is nothing
            // further to composite/present here. Calling rp_runtime_present() again would
            // silently advance the driven shim core an extra, un-synchronized frame per loop
            // iteration and desync it from the peer.
            rp::net::NetStatus st = netplay_session.tick(local);
            if (st == rp::net::NetStatus::Desync) {
                printf("DESYNC at frame %llu -- halting netplay\n", (unsigned long long)netplay_session.frame());
                fflush(stdout);
                break;
            }
            if (st == rp::net::NetStatus::Disconnected) {
                printf("peer disconnected -- halting netplay\n");
                fflush(stdout);
                break;
            }
        } else {
            rp_input_state local = read_local_input();
            rp_runtime_set_input(g_rt, 0, &local);   // host-owned input for driven AND presenting cores
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
    }
    rp_runtime_unload_core(g_rt);
    rp_runtime_destroy(g_rt);
    return 0;
}
