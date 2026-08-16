// RetroPark libretro shim — a driven core that LoadLibrary's an UNMODIFIED
// libretro core (e.g. FCEUmm), implements the libretro callback surface the
// core needs, converts its frames to RGBA8, and forwards them to RetroPark.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <retropark/retropark.h>
#include "PixelConvert.h"
#include "HwRenderGL.h"
#include "libretro.h"
#include "ShimInput.h"
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <utility>
#include <unordered_map>

#define RP_EXPORT extern "C" __declspec(dllexport)

namespace {

// One harvested core option. `values[0]` is the source of the default when the core's options API
// gives no explicit default_value (the legacy SET_VARIABLES path). `label` falls back to `value`
// when the core supplies no label (the v1/v2 definition path).
struct ShimOption {
    std::string key, desc, info, def;
    std::vector<std::pair<std::string,std::string>> values;  // (value, label); values[0] == default source
};

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
    // Core options harvested from whichever options API the wrapped core uses (legacy SET_VARIABLES or
    // the v1/v2 SET_CORE_OPTIONS[_INTL][_V2] descriptor forms), in the menu order the core declared them.
    // Populated once at retro_set_environment-driven negotiation, before retro_load_game; GET_VARIABLE
    // answers each key's default (see below), and core_options_json serializes the whole set.
    // (A3 adds: overrides map + dirty flag)
    std::vector<ShimOption> option_defs;                  // harvested, menu order
    std::unordered_map<std::string, size_t> option_index; // key -> index into option_defs
    std::string options_json_cache;                       // built lazily by core_options_json
    // A3: user overrides + live-update latch. option_overrides holds the values the frontend set
    // (empty => the core still runs on harvested defaults). options_dirty is raised on every set and
    // consumed (cleared) by the core's next GET_VARIABLE_UPDATE poll so it re-reads changed keys live.
    // get_scratch backs the const char* returned by GET_VARIABLE for the duration of that call.
    std::unordered_map<std::string, std::string> option_overrides;  // key -> user value
    bool options_dirty = false;                                     // GET_VARIABLE_UPDATE latch
    std::string get_scratch;                                        // stable storage for GET_VARIABLE's value
};

// Register one harvested option, deduping by key (first declaration wins, matching the libretro
// contract that a core declares each key once). An empty key is ignored.
void shim_add_option(Shim* s, const std::string& key, const std::string& desc, const std::string& info,
                     std::vector<std::pair<std::string,std::string>> values, const std::string& def) {
    if (key.empty() || s->option_index.count(key)) return;
    s->option_index[key] = s->option_defs.size();
    s->option_defs.push_back({key, desc, info, def, std::move(values)});
}

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
                auto it = g->option_index.find(v->key);
                if (it != g->option_index.end()) {
                    // Serve the user override when present, else the harvested default. The returned
                    // const char* must outlive this call, so stage it in a per-Shim scratch string.
                    auto ov = g->option_overrides.find(v->key);
                    g->get_scratch = (ov != g->option_overrides.end()) ? ov->second
                                                                       : g->option_defs[it->second].def;
                    v->value = g->get_scratch.c_str();
                    return true;
                }
            }
            v->value = nullptr;   // unknown key: no override, no default on file
            return false;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            // Report+consume the dirty latch: true once after a set(), so the core re-reads changed keys.
            *static_cast<bool*>(data) = g->options_dirty;
            g->options_dirty = false;
            return true;
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
            *static_cast<unsigned*>(data) = 2;   // shim understands the v2 core-options API (harvested below)
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
        // Legacy core-option negotiation. Each retro_variable::value is "<desc>; v1|v2|v3" with v1 as
        // the default (see first_option_value's comment for why the default matters — some cores hold
        // option-derived state, like sound volume, in a zero-init static that a "no such variable"
        // GET_VARIABLE answer never overwrites). Harvest the full definition (desc + values + default)
        // so core_options_json can serve it, not just the default token.
        case RETRO_ENVIRONMENT_SET_VARIABLES: {
            const auto* vars = static_cast<const retro_variable*>(data);
            for (; vars && vars->key; ++vars) {
                const std::string str = vars->value ? vars->value : "";
                std::string desc;
                std::vector<std::pair<std::string,std::string>> vals;
                const size_t semi = str.find(';');
                if (semi != std::string::npos) {
                    desc = str.substr(0, semi);
                    size_t i = semi + 1;
                    while (i < str.size() && str[i] == ' ') ++i;   // skip the single space after ';'
                    for (size_t start = i; start <= str.size(); ) {
                        const size_t bar = str.find('|', start);
                        const std::string tok = str.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
                        if (!tok.empty()) vals.emplace_back(tok, tok);   // legacy: label == value
                        if (bar == std::string::npos) break;
                        start = bar + 1;
                    }
                } else {
                    desc = str;   // spec-violating declaration with no ';': no parseable values/default
                }
                const std::string def = vals.empty() ? std::string() : vals.front().first;
                shim_add_option(g, vars->key, desc, "", std::move(vals), def);
            }
            return true;
        }
        // v1 core-options (RETRO_ENVIRONMENT_SET_CORE_OPTIONS[_INTL]). Each retro_core_option_definition
        // carries key/desc/info, an explicit default_value, and a value[]/label[] list terminated by a
        // null `value`. The _INTL form wraps the same definition array in `.us` (localized `.local`
        // ignored — the shim reports English).
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: {
            const retro_core_option_definition* d = nullptr;
            if (cmd == RETRO_ENVIRONMENT_SET_CORE_OPTIONS)
                d = static_cast<const retro_core_option_definition*>(data);
            else if (const auto* intl = static_cast<const retro_core_options_intl*>(data))
                d = intl->us;
            for (; d && d->key; ++d) {
                std::vector<std::pair<std::string,std::string>> vals;
                for (int i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX && d->values[i].value; ++i)
                    vals.emplace_back(d->values[i].value, d->values[i].label ? d->values[i].label : d->values[i].value);
                std::string def = d->default_value ? d->default_value : "";
                if (def.empty() && !vals.empty()) def = vals.front().first;
                shim_add_option(g, d->key, d->desc ? d->desc : "", d->info ? d->info : "", std::move(vals), def);
            }
            return true;
        }
        // v2 core-options (RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2[_INTL]). Same fields as v1 plus category
        // metadata (ignored); the definitions live under the struct's `.definitions`, and the _INTL form
        // wraps the v2 struct in `.us`.
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
            const retro_core_option_v2_definition* d = nullptr;
            if (cmd == RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2) {
                if (const auto* v2 = static_cast<const retro_core_options_v2*>(data)) d = v2->definitions;
            } else if (const auto* v2i = static_cast<const retro_core_options_v2_intl*>(data)) {
                if (v2i->us) d = v2i->us->definitions;
            }
            for (; d && d->key; ++d) {
                std::vector<std::pair<std::string,std::string>> vals;
                for (int i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX && d->values[i].value; ++i)
                    vals.emplace_back(d->values[i].value, d->values[i].label ? d->values[i].label : d->values[i].value);
                std::string def = d->default_value ? d->default_value : "";
                if (def.empty() && !vals.empty()) def = vals.front().first;
                shim_add_option(g, d->key, d->desc ? d->desc : "", d->info ? d->info : "", std::move(vals), def);
            }
            return true;
        }
        // Descriptor/registration commands: accept and ignore. The shim uses a fixed joypad map and
        // requeries av_info each present, so geometry / av-info updates need no bookkeeping here.
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
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
        if (g->hw && g->hw->zero_copy() && g->host.video_refresh_gl) {   // B2: hand the GL texture, no readback
            g->host.video_refresh_gl(g->host.host, g->hw->color_texture(), w, h,
                                     g->hw_cb.bottom_left_origin ? 1 : 0);   // honor the core's declared origin
        } else {                                                        // B1: read the FBO back to CPU RGBA
            uint32_t cw = w, ch = h, p = 0;
            const void* rgba = g->hw ? g->hw->read_frame(cw, ch, p) : nullptr;
            if (rgba) g->host.video_refresh(g->host.host, rgba, cw, ch, p);
            else      g->host.video_refresh(g->host.host, nullptr, w, h, 0);
        }
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

// Map RetroPark input to libretro, per port. keys[] (VK codes, the NES map) is OR'd with the
// generic abstract pad (pad_buttons/pad_axes) and ANALOG is answered from the abstract sticks; the
// pure mapping lives in ShimInput.h (shim_map_input) so it can be unit-tested without a DLL/GL.
// Both ports read the same rp_input_state[port]; netplay feeds each port a distinct snapshot, so
// port 0 and port 1 diverge by what the runtime stores, not by the mapping.
int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    if ((port != 0 && port != 1) || !g) return 0;
    return shim_map_input(g->input[port], device, index, id);
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
        // Mirror the runtime's max(max,base) so a malformed core with max<base still gets an FBO big enough
        // for the frames the runtime will accept (else every frame fails the runtime's pitch/size gate).
        // (Ternary, not std::max -- windows.h defines a max() macro here that would clobber it.)
        uint32_t mw = av.max_width > av.base_width ? av.max_width : av.base_width;
        uint32_t mh = av.max_height > av.base_height ? av.max_height : av.base_height;
        s->hw = std::make_unique<rp::HwRenderGL>();
        std::string e;
        // B2: if the host is GL, share its context so the FBO texture can be handed back zero-copy.
        // gl_share_context returns null on a non-GL host (D3D11/Vulkan) -> HwRenderGL runs standalone (B1).
        void* share = s->host.gl_share_context ? s->host.gl_share_context(s->host.host) : nullptr;
        if (!s->hw->setup(s->hw_cb.depth, s->hw_cb.stencil, s->hw_cb.bottom_left_origin,
                          mw, mh, (int)s->hw_cb.version_major, (int)s->hw_cb.version_minor, share, e)) {
            if (s->retro_unload_game) s->retro_unload_game();   // unwind the load the core already did
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
    // HW-render teardown: the core's GL cleanup (context_destroy + any GL frees inside retro_unload_game/
    // retro_deinit) must run against OUR GL context. Under the OpenGL host, composite_driven leaves the
    // HOST context current after each present, so without this the core would delete the host's GL objects
    // (per-context name spaces both start at 1) -> corrupted host rendering. Make ours current + fire the
    // core's context_destroy first (part of the libretro HW contract; GLideN64 hangs cleanup on it).
    if (s->hw) { s->hw->make_current(); if (s->hw_cb.context_destroy) s->hw_cb.context_destroy(); }
    if (s->game_loaded && s->retro_unload_game) s->retro_unload_game();
    if (s->retro_deinit) s->retro_deinit();
    if (s->lib) FreeLibrary(s->lib);
    if (g == s) g = nullptr;
    delete s;
}

// --- core_options_json (A2). Serialize the harvested option set as the ABI-documented JSON array
// [{key,desc,info,default,values:[{value,label}]}] ("[]" if none). Hand-rolled (no JSON lib in the
// shim); every string field is escaped. The result is cached on the Shim and stays valid until the
// core is destroyed. get/set land in A3 (their fptrs are NULL here). ---
void json_escape(const std::string& in, std::string& out) {
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;  case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;  case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                     else out += c;
        }
    }
}

const char* sh_core_options_json(rp_core* core) {
    Shim* s = reinterpret_cast<Shim*>(core);
    std::string& j = s->options_json_cache;
    j = "[";
    for (size_t i = 0; i < s->option_defs.size(); ++i) {
        const ShimOption& o = s->option_defs[i];
        if (i) j += ",";
        std::string k, d, inf, def; json_escape(o.key, k); json_escape(o.desc, d); json_escape(o.info, inf); json_escape(o.def, def);
        j += "{\"key\":\"" + k + "\",\"desc\":\"" + d + "\",\"info\":\"" + inf + "\",\"default\":\"" + def + "\",\"values\":[";
        for (size_t v = 0; v < o.values.size(); ++v) {
            if (v) j += ",";
            std::string vv, vl; json_escape(o.values[v].first, vv); json_escape(o.values[v].second, vl);
            j += "{\"value\":\"" + vv + "\",\"label\":\"" + vl + "\"}";
        }
        j += "]}";
    }
    j += "]";
    return j.c_str();
}

// --- core_option_get/set (A3). get returns the effective value (override else harvested default) for a
// known key, or NULL if the core never declared it. set records an override and raises the dirty latch so
// the running core re-reads the key on its next GET_VARIABLE_UPDATE. ---
const char* sh_core_option_get(rp_core* core, const char* key) {
    Shim* s = reinterpret_cast<Shim*>(core);
    auto it = s->option_index.find(key ? key : "");
    if (it == s->option_index.end()) return nullptr;
    auto ov = s->option_overrides.find(key);
    static thread_local std::string ret;   // stable lifetime for the returned c_str
    ret = (ov != s->option_overrides.end()) ? ov->second : s->option_defs[it->second].def;
    return ret.c_str();
}
rp_result sh_core_option_set(rp_core* core, const char* key, const char* value) {
    Shim* s = reinterpret_cast<Shim*>(core);
    if (!key || !value) return RP_ERR_BAD_ARG;
    if (!s->option_index.count(key)) return RP_ERR_NOT_FOUND;
    s->option_overrides[key] = value;
    s->options_dirty = true;
    return RP_OK;
}

const rp_core_abi kAbi = {
    RETROPARK_ABI_VERSION, sh_get_info, sh_create, sh_destroy,
    nullptr, nullptr, nullptr,          // set_surfaces, start, stop
    sh_get_av_info, sh_run_frame,
    sh_serialize_size, sh_serialize, sh_unserialize,
    sh_load_content,
    sh_core_options_json, sh_core_option_get, sh_core_option_set   // full A3 option channel
};

} // namespace

RP_EXPORT const rp_core_abi* rp_get_core_abi(void) { return &kAbi; }
