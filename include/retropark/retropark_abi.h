#ifndef RETROPARK_ABI_H
#define RETROPARK_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RETROPARK_ABI_VERSION 1u
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
    RP_GFX_VULKAN = 1           /* later slice */
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

typedef struct rp_input_state {
    uint8_t  keys[256];         /* virtual-key down flags */
    int16_t  pad_axes[8];
    uint16_t pad_buttons;
} rp_input_state;

typedef struct rp_host_iface {
    rp_host* host;
    void (*log)(rp_host* host, int level, const char* msg);
    void (*submit_frame)(rp_host* host, uint32_t index, uint64_t generation);
    void (*input_state)(rp_host* host, rp_input_state* out);
} rp_host_iface;

typedef struct rp_core_info {
    uint32_t        abi_version;
    rp_core_type    type;
    rp_graphics_api graphics_api;
    const char*     id;
} rp_core_info;

typedef struct rp_core_abi {
    uint32_t abi_version;                       /* must equal RETROPARK_ABI_VERSION */
    void      (*get_info)(rp_core_info* out);
    rp_core*  (*create)(const rp_host_iface* host);
    void      (*destroy)(rp_core* core);
    rp_result (*set_surfaces)(rp_core* core, const rp_surface_desc* descs, uint32_t count);
    rp_result (*start)(rp_core* core);
    rp_result (*stop)(rp_core* core);
} rp_core_abi;

typedef const rp_core_abi* (*rp_get_core_abi_fn)(void);

#ifdef __cplusplus
}
#endif

#endif /* RETROPARK_ABI_H */
