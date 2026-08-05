#ifndef RETROPARK_H
#define RETROPARK_H

#include <stdint.h>
#include "retropark_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rp_runtime rp_runtime;

rp_runtime* rp_runtime_create(rp_graphics_api api, void* native_window);
void        rp_runtime_destroy(rp_runtime* rt);

rp_result   rp_runtime_load_core(rp_runtime* rt, const char* core_dir);
rp_result   rp_runtime_unload_core(rp_runtime* rt);

rp_result   rp_runtime_resize(rp_runtime* rt, uint32_t width, uint32_t height);
void        rp_runtime_set_input(rp_runtime* rt, const rp_input_state* in);

/* Composite latest core frame + overlay; if out_rgba != NULL copies the RGBA8 image. */
rp_result   rp_runtime_present(rp_runtime* rt, uint8_t* out_rgba);

#ifdef __cplusplus
}
#endif
#endif /* RETROPARK_H */
