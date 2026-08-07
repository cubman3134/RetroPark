// RetroPark libretro shim — a driven core that LoadLibrary's an UNMODIFIED
// libretro core (e.g. FCEUmm), implements the libretro callback surface the
// core needs, converts its frames to RGBA8, and forwards them to RetroPark.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <retropark/retropark.h>
#include "PixelConvert.h"
#include "libretro.h"
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <cstdarg>

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
    // state
    unsigned pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;   // libretro default
    std::vector<uint8_t> rgba;      // converted frame
    std::vector<uint8_t> rom;       // content buffer
    std::string sys_dir;            // returned to the core (writable path)
    bool game_loaded = false;
    rp_input_state input{};         // last input snapshot from RetroPark
};

Shim* g = nullptr;   // single active shim instance (libretro callbacks are global C fns)

// No-op logger handed to the core via GET_LOG_INTERFACE. Variadic, cdecl to match
// retro_log_printf_t. Discards everything; the shim is headless.
void RETRO_CALLCONV shim_log(enum retro_log_level, const char*, ...) {}

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
            v->value = nullptr;   // no overrides: core uses its own defaults
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
        // Descriptor/registration commands: accept and ignore. The shim uses a
        // fixed NES joypad map and requeries av_info each present, so geometry /
        // av-info updates need no bookkeeping here.
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_VARIABLES:
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
        case RETRO_ENVIRONMENT_SET_HW_RENDER:
            return false;   // force software rendering
        default:
            return false;
    }
}

void video_cb(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (!data || w == 0 || h == 0) {   // duplicate frame
        g->host.video_refresh(g->host.host, nullptr, w, h, 0);
        return;
    }
    g->rgba.assign((size_t)w * h * 4, 0);
    rp::convert_to_rgba8(data, w, h, (uint32_t)pitch, g->pixel_format, g->rgba.data());
    g->host.video_refresh(g->host.host, g->rgba.data(), w, h, w * 4);
}

void input_poll_cb() {
    if (g) g->host.input_state(g->host.host, &g->input);
}

int16_t input_state_cb(unsigned port, unsigned device, unsigned, unsigned id) {
    if (port != 0 || device != RETRO_DEVICE_JOYPAD || !g) return 0;
    // Map RetroPark rp_input_state.keys[] (VK codes) to NES buttons.
    switch (id) {
        case RETRO_DEVICE_ID_JOYPAD_UP:     return g->input.keys[VK_UP]     ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_DOWN:   return g->input.keys[VK_DOWN]   ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_LEFT:   return g->input.keys[VK_LEFT]   ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return g->input.keys[VK_RIGHT]  ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_A:      return g->input.keys['X']       ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_B:      return g->input.keys['Z']       ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_START:  return g->input.keys[VK_RETURN] ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_SELECT: return g->input.keys[VK_SHIFT]  ? 1 : 0;
        default: return 0;
    }
}

void   audio_cb(int16_t, int16_t) {}                                   // dropped
size_t audio_batch_cb(const int16_t*, size_t frames) { return frames; } // dropped

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

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
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
    s->lib = LoadLibraryW(core_dll.c_str());
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

    if (!s->retro_api_version || s->retro_api_version() != RETRO_API_VERSION ||
        !s->retro_run || !s->retro_load_game || !s->retro_set_environment ||
        !s->retro_set_video_refresh || !s->retro_init ||
        !s->retro_get_system_info || !s->retro_get_system_av_info) {
        FreeLibrary(s->lib);
        delete s; g = nullptr;
        return nullptr;
    }

    s->sys_dir = to_utf8(dir);   // real writable path for system/save/assets
    s->retro_set_environment(env_cb);
    s->retro_set_video_refresh(video_cb);
    s->retro_set_input_poll(input_poll_cb);
    s->retro_set_input_state(input_state_cb);
    s->retro_set_audio_sample(audio_cb);
    s->retro_set_audio_sample_batch(audio_batch_cb);
    s->retro_init();
    return reinterpret_cast<rp_core*>(s);
}

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
    if (s->game_loaded) s->retro_run();
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
    nullptr, nullptr, nullptr,          // serialize_size, serialize, unserialize
    sh_load_content
};

} // namespace

RP_EXPORT const rp_core_abi* rp_get_core_abi(void) { return &kAbi; }
