#ifndef RETROPARK_ABI_H
#define RETROPARK_ABI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RETROPARK_ABI_VERSION 8u  /* was 7 -- rp_host_iface gains gl_share_context + video_refresh_gl (B2 zero-copy GL) */
#define RP_CORE_ABI_EXPORT_NAME "rp_get_core_abi"

typedef enum rp_result {
    RP_OK = 0,
    RP_ERR_ABI_MISMATCH = 1,
    RP_ERR_BAD_ARG = 2,
    RP_ERR_DEVICE = 3,
    RP_ERR_INTERNAL = 4,
    RP_ERR_TIMEOUT = 5,
    RP_ERR_NOT_FOUND = 6,
    RP_ERR_UNSUPPORTED = 7
} rp_result;

typedef enum rp_core_type {
    RP_CORE_PRESENTING = 0,
    RP_CORE_DRIVEN = 1          /* declared; not implemented in Slice A */
} rp_core_type;

typedef enum rp_graphics_api {
    RP_GFX_D3D11 = 0,
    RP_GFX_VULKAN = 1,
    RP_GFX_NONE = 2,            /* driven cores: no host-managed swapchain */
    RP_GFX_OPENGL = 3          /* OpenGL host compositor (driven cores). Additive: no ABI-version bump. */
} rp_graphics_api;

typedef enum rp_pixel_format {
    RP_FMT_R8G8B8A8_UNORM = 0
} rp_pixel_format;

typedef struct rp_host rp_host;   /* opaque host handle */
typedef struct rp_core rp_core;   /* opaque core instance */

typedef struct rp_surface_desc {
    uint32_t index;             /* slot index in the ring */
    uint32_t width;
    uint32_t height;
    uint32_t format;            /* rp_pixel_format */
    void*    shared_handle;     /* NT HANDLE to a shared, keyed-mutex Texture2D (D3D11) */
    uint64_t generation;        /* ring generation; echoed back on submit_frame */
} rp_surface_desc;

typedef struct rp_surface_set {
    uint32_t               count;
    uint32_t               reserved;
    const rp_surface_desc* surfaces;
    void*                  sync_handle;      /* PRODUCE timeline semaphore NT handle (Vulkan): core signals 2f+2 per
                                                frame, host waits it before compositing; NULL for D3D11 */
    uint8_t                device_uuid[16];  /* target VkPhysicalDevice UUID (Vulkan); all-zero for D3D11 */
    void*                  consume_sync_handle; /* CONSUME timeline semaphore NT handle (Vulkan): the HOST owns +
                                                signals it (2f+3) after its GPU finishes reading frame f, and the
                                                core ONLY waits on it before REUSING a slot. This is the genuine
                                                GPU-read-before-write back-pressure for N>=2 shared images: unlike a
                                                single shared timeline (which the core's own produce signals also
                                                advance, so the reuse wait is satisfied by the core itself), a
                                                one-directional channel the core never signals cannot be self-
                                                satisfied. 0/NULL => old single-timeline lock-step (slot_count==1
                                                boot path + non-Vulkan cores are unaffected). */
} rp_surface_set;

typedef struct rp_input_state {
    uint8_t  keys[256];         /* virtual-key down flags */
    int16_t  pad_axes[8];
    uint16_t pad_buttons;
} rp_input_state;

/* rp_input_state.pad_buttons bit indices (generic abstract pad; a bit is (1u << RP_PAD_x)). */
#define RP_PAD_A          0
#define RP_PAD_B          1
#define RP_PAD_X          2
#define RP_PAD_Y          3
#define RP_PAD_L          4   /* left shoulder (digital) */
#define RP_PAD_R          5   /* right shoulder (digital) */
#define RP_PAD_SELECT     6
#define RP_PAD_START      7
#define RP_PAD_L3         8   /* left stick click */
#define RP_PAD_R3         9   /* right stick click */
#define RP_PAD_DPAD_UP    10
#define RP_PAD_DPAD_DOWN  11
#define RP_PAD_DPAD_LEFT  12
#define RP_PAD_DPAD_RIGHT 13
#define RP_PAD_GUIDE      14
/* rp_input_state.pad_axes[] indices. Sticks -32768..32767 (Y up = positive); triggers 0..32767. */
#define RP_AXIS_LEFT_X        0
#define RP_AXIS_LEFT_Y        1
#define RP_AXIS_RIGHT_X       2
#define RP_AXIS_RIGHT_Y       3
#define RP_AXIS_LEFT_TRIGGER  4
#define RP_AXIS_RIGHT_TRIGGER 5

typedef struct rp_av_info {
    double   fps;
    double   sample_rate;
    uint32_t base_width, base_height, max_width, max_height;
    uint32_t pixel_format;      /* rp_pixel_format */
} rp_av_info;

typedef struct rp_host_iface {
    rp_host* host;
    void (*log)(rp_host* host, int level, const char* msg);
    void (*submit_frame)(rp_host* host, uint32_t index, uint64_t generation, uint64_t sync_value);
    void (*input_state)(rp_host* host, uint32_t port, rp_input_state* out);
    void (*video_refresh)(rp_host* host, const void* data, uint32_t width, uint32_t height, uint32_t pitch);
    void (*audio_sample)(rp_host* host, const int16_t* frames, size_t num_frames);
    /* Audio flow control for PRESENTING cores (Dolphin/RPCS3) that pull their own mixer off a thread:
       returns how many stereo frames the host can accept right now to reach its target output-queue
       depth (0 = the queue is full, pull nothing). Pull exactly this many, forward via audio_sample,
       and the average pull rate self-locks to the host's true playback clock -- no free-running timer,
       no under/overrun crackle. Driven cores (host owns the frame clock) do not use this. */
    size_t (*audio_want)(rp_host* host);
    /* B2 zero-copy GL (OpenGL host compositor + GL-producing driven cores). gl_share_context returns the
       host's GL context handle (HGLRC on Win32) a core can share texture objects with (NULL if the host
       backend is not GL). video_refresh_gl hands the host an already-GPU-resident GL texture (from the
       shared context) to composite directly -- no CPU readback -- with its dimensions and origin
       convention (bottom_left_origin != 0 => GL's native lower-left origin). Non-GL cores keep using
       video_refresh (CPU path); the two are mutually exclusive per frame. */
    void* (*gl_share_context)(rp_host* host);
    void  (*video_refresh_gl)(rp_host* host, unsigned gl_texture, uint32_t width, uint32_t height, int bottom_left_origin);
} rp_host_iface;

typedef struct rp_core_info {
    uint32_t    abi_version;
    uint32_t    type;           /* rp_core_type */
    uint32_t    graphics_api;   /* rp_graphics_api */
    const char* id;
} rp_core_info;

typedef struct rp_core_abi {
    uint32_t abi_version;                       /* must equal RETROPARK_ABI_VERSION */
    void      (*get_info)(rp_core_info* out);
    rp_core*  (*create)(const rp_host_iface* host);
    void      (*destroy)(rp_core* core);
    rp_result (*set_surfaces)(rp_core* core, const rp_surface_set* set);
    rp_result (*start)(rp_core* core);
    rp_result (*stop)(rp_core* core);
    void      (*get_av_info)(rp_core* core, rp_av_info* out);
    void      (*run_frame)(rp_core* core);
    size_t    (*serialize_size)(rp_core* core);
    rp_result (*serialize)(rp_core* core, void* data, size_t size);
    rp_result (*unserialize)(rp_core* core, const void* data, size_t size);
    rp_result (*load_content)(rp_core* core, const char* path);
} rp_core_abi;

typedef const rp_core_abi* (*rp_get_core_abi_fn)(void);

#ifdef __cplusplus
}
#endif

#endif /* RETROPARK_ABI_H */
