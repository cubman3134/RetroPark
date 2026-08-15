// RetroPark libretro shim — a driven core that LoadLibrary's an UNMODIFIED
// libretro core (e.g. FCEUmm), implements the libretro callback surface the
// core needs, converts its frames to RGBA8, and forwards them to RetroPark.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <retropark/retropark.h>
#include "PixelConvert.h"
#include "HwRenderGL.h"
#include "libretro.h"
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <cstdarg>
#include <memory>
#include <unordered_map>

#define RP_EXPORT extern "C" __declspec(dllexport)

namespace {

struct Shim {
    rp_host_iface host{};
    HMODULE lib = nullptr;
    // libretro fn pointers
    void     (*retro_init)() = nullptr;
    void     (*retro_deinit)() = nullptr;
    unsigned (*retro_api_version)() = nullptr;
    void     (*retro_get_system_info)(retro_system_info*) = nullptr;
    void     (*retro_get_system_av_info)(retro_system_av_info*) = nullptr;
    void     (*retro_set_environment)(retro_environment_t) = nullptr;
    void     (*retro_set_video_refresh)(retro_video_refresh_t) = nullptr;
    void     (*retro_set_audio_sample)(retro_audio_sample_t) = nullptr;
    void     (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t) = nullptr;
    void     (*retro_set_input_poll)(retro_input_poll_t) = nullptr;
    void     (*retro_set_input_state)(retro_input_state_t) = nullptr;
    bool     (*retro_load_game)(const retro_game_info*) = nullptr;
    void     (*retro_unload_game)() = nullptr;
    void     (*retro_run)() = nullptr;
    size_t   (*retro_serialize_size)() = nullptr;
    bool     (*retro_serialize)(void*, size_t) = nullptr;
    bool     (*retro_unserialize)(const void*, size_t) = nullptr;
    // state
    unsigned pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;   // libretro default
    std::vector<uint8_t> rgba;      // converted frame
    std::vector<uint8_t> rom;       // content buffer
    std::string sys_dir;            // returned to the core (writable path)
    bool game_loaded = false;
    // HW-render (B1). Set when a core requests desktop-GL SET_HW_RENDER; the shim renders the core into
    // HwRenderGL's FBO and reads it back to CPU RGBA, so the runtime still sees a driven core.
    retro_hw_render_callback hw_cb{};
    bool hw_requested = false;
    std::unique_ptr<rp::HwRenderGL> hw;
    rp_input_state input[2]{};      // last input snapshot from RetroPark, per port
    // Core option defaults captured from RETRO_ENVIRONMENT_SET_VARIABLES (legacy variable
    // API), keyed by option key. Populated once at retro_set_environment-driven negotiation,
    // before retro_load_game; answered back verbatim on GET_VARIABLE (see below).
    std::unordered_map<std::string, std::string> option_defaults;
};

// RETRO_ENVIRONMENT_SET_VARIABLES's retro_variable::value is, per the libretro spec, a
// "<human title>; <default>|<opt2>|<opt3>..." string. Many cores (FCEUmm's sound volume
// among them) hold their internal option state in a plain static that is zero-initialized
// until the frontend actually answers a later GET_VARIABLE for that key with a real value —
// a frontend that always reports "no such variable" leaves those statics at their zero
// default forever (e.g. FCEUmm's sndvolume stays 0 => silent output despite a real audio
// stream being paced and forwarded). Extract the "<default>" token so GET_VARIABLE can hand
// it back and cores initialize the way any real libretro frontend would leave them.
//
// Returns false (out untouched) when 'value' has no ';' separator — a malformed,
// spec-violating declaration with no parseable default token. The caller must NOT store
// an entry for such a key: GET_VARIABLE needs to fall through to its "no such variable"
// answer (value=nullptr, false) rather than hand the core a bogus empty-string override.
// Well-formed libretro declarations always contain ';', so this never affects a
// conforming core's real defaults.
bool first_option_value(const std::string& value, std::string& out) {
    size_t semi = value.find(';');
    if (semi == std::string::npos) return false;
    size_t start = semi + 1;
    while (start < value.size() && value[start] == ' ') ++start;
    size_t bar = value.find('|', start);
    out = value.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
    return true;
}

// libretro's callbacks (env_cb, video_cb, input_poll_cb, ...) are global C functions with
// no per-instance context param, so the shim supports exactly one active instance at a
// time; a second concurrent sh_create() would reroute the first core's frames/input here.
Shim* g = nullptr;

// No-op logger handed to the core via GET_LOG_INTERFACE. Variadic, cdecl to match
// retro_log_printf_t. Discards everything; the shim is headless.
void RETRO_CALLCONV shim_log(enum retro_log_level, const char*, ...) {}

// --- HW-render frontend GL getters (B1). Handed to the core in the SET_HW_RENDER struct so it can
// render into our FBO and load its own GL entrypoints. RETRO_CALLCONV to match the libretro typedefs. ---
// get_current_framebuffer: the FBO the core renders into (0 before setup / for SW cores).
uintptr_t RETRO_CALLCONV hw_get_current_framebuffer() { return g && g->hw ? g->hw->fbo_id() : 0; }
// get_proc_address: the core loads its OWN GL through this (wgl first, opengl32 fallback for GL 1.1 syms).
retro_proc_address_t RETRO_CALLCONV hw_get_proc_address(const char* sym) {
    if (!sym) return nullptr;
    void* p = (void*)wglGetProcAddress(sym);
    if (p == nullptr || p == (void*)0x1 || p == (void*)0x2 || p == (void*)0x3 || p == (void*)-1) {
        static HMODULE gl = GetModuleHandleA("opengl32.dll");
        p = (void*)GetProcAddress(gl, sym);
    }
    return reinterpret_cast<retro_proc_address_t>(p);
}

// --- libretro environment callback (C, must be global) ---
bool env_cb(unsigned cmd, void* data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            g->pixel_format = *static_cast<const unsigned*>(data);
            return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *static_cast<bool*>(data) = true;
            return true;
        case RETRO_ENVIRONMENT_GET_OVERSCAN:
            *static_cast<bool*>(data) = false;   // crop nothing; core decides
            return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
            *static_cast<const char**>(data) = g->sys_dir.c_str();
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            auto* v = static_cast<retro_variable*>(data);
            if (v->key) {
                auto it = g->option_defaults.find(v->key);
                if (it != g->option_defaults.end()) {
                    v->value = it->second.c_str();
                    return true;
                }
            }
            v->value = nullptr;   // unknown key: no override, no default on file
            return false;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *static_cast<bool*>(data) = false;
            return true;
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
            *static_cast<unsigned*>(data) = 0;   // legacy variable interface only
            return true;
        case RETRO_ENVIRONMENT_GET_LANGUAGE:
            *static_cast<unsigned*>(data) = RETRO_LANGUAGE_ENGLISH;
            return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            static_cast<retro_log_callback*>(data)->log = shim_log;
            return true;
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            return false;   // core falls back to per-id input_state polling
        case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
            *static_cast<int*>(data) = 0x3;   // bit0 video, bit1 audio both enabled
            return true;
        case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
            *static_cast<bool*>(data) = false;
            return true;
        // Legacy core-option negotiation: capture each option's default token so a later
        // GET_VARIABLE can answer it (see first_option_value's comment for why this matters
        // — some cores hold option-derived state, like sound volume, in a zero-init static
        // that a "no such variable" GET_VARIABLE answer never overwrites).
        case RETRO_ENVIRONMENT_SET_VARIABLES: {
            const auto* vars = static_cast<const retro_variable*>(data);
            if (vars) {
                for (; vars->key; ++vars) {
                    std::string def;
                    if (vars->value && first_option_value(vars->value, def)) {
                        g->option_defaults[vars->key] = def;
                    }
                }
            }
            return true;
        }
        // Descriptor/registration commands: accept and ignore. The shim uses a
        // fixed NES joypad map and requeries av_info each present, so geometry /
        // av-info updates need no bookkeeping here. GET_CORE_OPTIONS_VERSION reports 0
        // above, so a well-behaved core negotiates options via the legacy SET_VARIABLES
        // path handled above rather than these v1/v2 descriptor forms; still accept them
        // (return true, do nothing) in case a core calls them anyway.
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
        case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
        case RETRO_ENVIRONMENT_SET_GEOMETRY:
        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
            return true;
        case RETRO_ENVIRONMENT_SET_HW_RENDER: {
            auto* cb = static_cast<retro_hw_render_callback*>(data);
            if (!cb) return false;
            // Desktop GL only (B1). GLES/Vulkan/etc. -> reject; the core falls back to SW or fails.
            if (cb->context_type != RETRO_HW_CONTEXT_OPENGL &&
                cb->context_type != RETRO_HW_CONTEXT_OPENGL_CORE)
                return false;
            g->hw_cb = *cb;
            g->hw_requested = true;
            g->hw_cb.get_current_framebuffer = hw_get_current_framebuffer;
            g->hw_cb.get_proc_address = hw_get_proc_address;
            // The core reads get_current_framebuffer/get_proc_address back from ITS struct, so write them
            // into the caller's struct too:
            cb->get_current_framebuffer = hw_get_current_framebuffer;
            cb->get_proc_address = hw_get_proc_address;
            return true;
        }
        default:
            return false;
    }
}

void video_cb(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (data == RETRO_HW_FRAME_BUFFER_VALID) {          // HW-render: the core drew into our FBO
        uint32_t p = 0;
        const void* rgba = g->hw ? g->hw->read_frame(w, h, p) : nullptr;
        if (rgba) g->host.video_refresh(g->host.host, rgba, w, h, p);
        else      g->host.video_refresh(g->host.host, nullptr, w, h, 0);
        return;
    }
    if (!data || w == 0 || h == 0) {   // duplicate frame (SW or HW dupe)
        g->host.video_refresh(g->host.host, nullptr, w, h, 0);
        return;
    }
    g->rgba.assign((size_t)w * h * 4, 0);
    rp::convert_to_rgba8(data, w, h, (uint32_t)pitch, g->pixel_format, g->rgba.data());
    g->host.video_refresh(g->host.host, g->rgba.data(), w, h, w * 4);
}

void input_poll_cb() {
    if (g) {
        g->host.input_state(g->host.host, 0, &g->input[0]);
        g->host.input_state(g->host.host, 1, &g->input[1]);
    }
}

// Map RetroPark rp_input_state.keys[] (VK codes) to NES buttons, per port. Both ports
// currently read the same VK keys -- that's fine; netplay feeds each port a distinct
// rp_input_state, so port 0 and port 1 diverge by what the runtime stores, not by key
// mapping. Local keyboard control of P2 in the harness is out of scope.
int16_t input_state_cb(unsigned port, unsigned device, unsigned, unsigned id) {
    if ((port != 0 && port != 1) || device != RETRO_DEVICE_JOYPAD || !g) return 0;
    const rp_input_state& in = g->input[port];
    switch (id) {
        case RETRO_DEVICE_ID_JOYPAD_UP:     return in.keys[VK_UP]     ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_DOWN:   return in.keys[VK_DOWN]   ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_LEFT:   return in.keys[VK_LEFT]   ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return in.keys[VK_RIGHT]  ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_A:      return in.keys['X']       ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_B:      return in.keys['Z']       ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_START:  return in.keys[VK_RETURN] ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_SELECT: return in.keys[VK_SHIFT]  ? 1 : 0;
        default: return 0;
    }
}

// per-sample: forward one stereo frame.
void audio_cb(int16_t left, int16_t right) {
    if (g) { int16_t pair[2] = {left, right}; g->host.audio_sample(g->host.host, pair, 1); }
}
// batch: interleaved stereo int16, `frames` stereo frames. Forward straight through.
size_t audio_batch_cb(const int16_t* data, size_t frames) {
    if (g && data && frames) g->host.audio_sample(g->host.host, data, frames);
    return frames;
}

// Directory of THIS dll (used to self-locate core.json and the libretro core).
std::wstring shim_dir() {
    wchar_t path[MAX_PATH]{};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&shim_dir), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring s(path);
    return s.substr(0, s.find_last_of(L"\\/"));
}

std::string to_ansi(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

// libretro cores fopen() the system/save/assets directory as an ANSI (CP_ACP) path, not
// UTF-8, so a non-ASCII install directory would otherwise break save/system file access.
// Prefer the 8.3 short path (always ASCII-representable) converted to ANSI; fall back to
// converting the long path directly to ANSI if short names are unavailable (e.g. disabled
// on the volume) or on the rare case that fails too.
std::string ansi_safe_dir(const std::wstring& dir) {
    wchar_t shortp[MAX_PATH]{};
    DWORD n = GetShortPathNameW(dir.c_str(), shortp, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return to_ansi(std::wstring(shortp, n));
    return to_ansi(dir);
}

// Minimal, dependency-free parse of the "libretro_core" value from the sibling
// core.json. Returns the core dll filename, or the FCEUmm default if absent.
std::wstring core_dll_name(const std::wstring& dir) {
    const std::wstring fallback = L"fceumm_libretro.dll";
    std::ifstream f(dir + L"\\core.json", std::ios::binary);
    if (!f) return fallback;
    std::stringstream ss; ss << f.rdbuf();
    const std::string text = ss.str();
    const std::string key = "\"libretro_core\"";
    size_t k = text.find(key);
    if (k == std::string::npos) return fallback;
    size_t colon = text.find(':', k + key.size());
    if (colon == std::string::npos) return fallback;
    size_t q1 = text.find('"', colon);
    if (q1 == std::string::npos) return fallback;
    size_t q2 = text.find('"', q1 + 1);
    if (q2 == std::string::npos) return fallback;
    std::string name = text.substr(q1 + 1, q2 - q1 - 1);
    if (name.empty()) return fallback;
    return std::wstring(name.begin(), name.end());   // ASCII dll name
}

template<class T> void load_fn(Shim* s, T& fn, const char* name) {
    fn = reinterpret_cast<T>(GetProcAddress(s->lib, name));
}

// --- RetroPark ABI ---
void sh_get_info(rp_core_info* out) {
    out->abi_version = RETROPARK_ABI_VERSION;
    out->type = RP_CORE_DRIVEN;
    out->graphics_api = RP_GFX_NONE;
    out->id = "libretro_shim";
}

rp_core* sh_create(const rp_host_iface* host) {
    auto* s = new Shim();
    s->host = *host;
    g = s;

    const std::wstring dir = shim_dir();
    const std::wstring core_dll = dir + L"\\" + core_dll_name(dir);
    // Search DLLs the core's own dir + safe system dirs (defense-in-depth against
    // DLL planting); core_dll itself is already an absolute self-located path.
    s->lib = LoadLibraryExW(core_dll.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (!s->lib) { delete s; g = nullptr; return nullptr; }

    load_fn(s, s->retro_api_version, "retro_api_version");
    load_fn(s, s->retro_init, "retro_init");
    load_fn(s, s->retro_deinit, "retro_deinit");
    load_fn(s, s->retro_get_system_info, "retro_get_system_info");
    load_fn(s, s->retro_get_system_av_info, "retro_get_system_av_info");
    load_fn(s, s->retro_set_environment, "retro_set_environment");
    load_fn(s, s->retro_set_video_refresh, "retro_set_video_refresh");
    load_fn(s, s->retro_set_audio_sample, "retro_set_audio_sample");
    load_fn(s, s->retro_set_audio_sample_batch, "retro_set_audio_sample_batch");
    load_fn(s, s->retro_set_input_poll, "retro_set_input_poll");
    load_fn(s, s->retro_set_input_state, "retro_set_input_state");
    load_fn(s, s->retro_load_game, "retro_load_game");
    load_fn(s, s->retro_unload_game, "retro_unload_game");
    load_fn(s, s->retro_run, "retro_run");
    load_fn(s, s->retro_serialize_size, "retro_serialize_size");
    load_fn(s, s->retro_serialize, "retro_serialize");
    load_fn(s, s->retro_unserialize, "retro_unserialize");

    if (!s->retro_api_version || s->retro_api_version() != RETRO_API_VERSION ||
        !s->retro_run || !s->retro_load_game || !s->retro_set_environment ||
        !s->retro_set_video_refresh || !s->retro_init ||
        !s->retro_get_system_info || !s->retro_get_system_av_info) {
        FreeLibrary(s->lib);
        delete s; g = nullptr;
        return nullptr;
    }

    s->sys_dir = ansi_safe_dir(dir);   // ANSI-safe path for system/save/assets (see ansi_safe_dir)
    s->retro_set_environment(env_cb);
    s->retro_set_video_refresh(video_cb);
    s->retro_set_input_poll(input_poll_cb);
    s->retro_set_input_state(input_state_cb);
    s->retro_set_audio_sample(audio_cb);
    s->retro_set_audio_sample_batch(audio_batch_cb);
    s->retro_init();
    return reinterpret_cast<rp_core*>(s);
}

void sh_get_av_info(rp_core* core, rp_av_info* out);   // fwd: HW-render setup below queries max geometry

rp_result sh_load_content(rp_core* core, const char* path) {
    auto* s = reinterpret_cast<Shim*>(core);
    if (!path || !*path) return RP_ERR_BAD_ARG;
    std::ifstream f(path, std::ios::binary);
    if (!f) return RP_ERR_NOT_FOUND;
    s->rom.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());

    retro_system_info si{};
    s->retro_get_system_info(&si);
    retro_game_info gi{};
    gi.path = path;
    if (!si.need_fullpath) {
        gi.data = s->rom.data();
        gi.size = s->rom.size();
    }
    if (!s->retro_load_game(&gi)) return RP_ERR_UNSUPPORTED;
    s->game_loaded = true;

    // HW-render (B1): the core requested a desktop-GL context during set_environment. Now that geometry
    // is known, stand up HwRenderGL's FBO and fire context_reset so the core builds its GL objects.
    if (s->hw_requested) {
        rp_av_info av{}; sh_get_av_info(core, &av);     // reuse the shim's av-info query for max geometry
        uint32_t mw = av.max_width ? av.max_width : av.base_width;
        uint32_t mh = av.max_height ? av.max_height : av.base_height;
        s->hw = std::make_unique<rp::HwRenderGL>();
        std::string e;
        if (!s->hw->setup(s->hw_cb.depth, s->hw_cb.stencil, s->hw_cb.bottom_left_origin,
                          mw, mh, (int)s->hw_cb.version_major, (int)s->hw_cb.version_minor, e)) {
            s->hw.reset(); s->game_loaded = false;
            return RP_ERR_DEVICE;                        // HW core can't run without its GL context
        }
        s->hw->make_current();
        if (s->hw_cb.context_reset) s->hw_cb.context_reset();   // core builds its GL objects
    }
    return RP_OK;
}

void sh_get_av_info(rp_core* core, rp_av_info* out) {
    auto* s = reinterpret_cast<Shim*>(core);
    retro_system_av_info av{};
    s->retro_get_system_av_info(&av);
    out->fps = av.timing.fps;
    out->sample_rate = av.timing.sample_rate;
    out->base_width = av.geometry.base_width;
    out->base_height = av.geometry.base_height;
    out->max_width = av.geometry.max_width;
    out->max_height = av.geometry.max_height;
    out->pixel_format = RP_FMT_R8G8B8A8_UNORM;   // shim always outputs RGBA8
}

void sh_run_frame(rp_core* core) {
    auto* s = reinterpret_cast<Shim*>(core);
    if (s->game_loaded) {
        if (s->hw) s->hw->make_current();   // HW cores render into our FBO under this context
        s->retro_run();
    }
}

size_t sh_serialize_size(rp_core* core) {
    auto* s = reinterpret_cast<Shim*>(core);
    return s->retro_serialize_size ? s->retro_serialize_size() : 0;
}

rp_result sh_serialize(rp_core* core, void* data, size_t size) {
    auto* s = reinterpret_cast<Shim*>(core);
    if (!s->retro_serialize) return RP_ERR_UNSUPPORTED;
    return s->retro_serialize(data, size) ? RP_OK : RP_ERR_UNSUPPORTED;
}

rp_result sh_unserialize(rp_core* core, const void* data, size_t size) {
    auto* s = reinterpret_cast<Shim*>(core);
    if (!s->retro_unserialize) return RP_ERR_UNSUPPORTED;
    return s->retro_unserialize(data, size) ? RP_OK : RP_ERR_UNSUPPORTED;
}

void sh_destroy(rp_core* core) {
    auto* s = reinterpret_cast<Shim*>(core);
    if (s->game_loaded && s->retro_unload_game) s->retro_unload_game();
    if (s->retro_deinit) s->retro_deinit();
    if (s->lib) FreeLibrary(s->lib);
    if (g == s) g = nullptr;
    delete s;
}

const rp_core_abi kAbi = {
    RETROPARK_ABI_VERSION, sh_get_info, sh_create, sh_destroy,
    nullptr, nullptr, nullptr,          // set_surfaces, start, stop
    sh_get_av_info, sh_run_frame,
    sh_serialize_size, sh_serialize, sh_unserialize,
    sh_load_content
};

} // namespace

RP_EXPORT const rp_core_abi* rp_get_core_abi(void) { return &kAbi; }
