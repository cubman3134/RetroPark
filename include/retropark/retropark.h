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
rp_result   rp_runtime_load_content(rp_runtime* rt, const char* path);

rp_result   rp_runtime_resize(rp_runtime* rt, uint32_t width, uint32_t height);
void        rp_runtime_set_input(rp_runtime* rt, const rp_input_state* in);

/* Composite latest core frame + overlay; if out_rgba != NULL copies the RGBA8 image. */
rp_result   rp_runtime_present(rp_runtime* rt, uint8_t* out_rgba);

/* Diagnostics for the audio path (test/telemetry): frames_out = total stereo frames the runtime
   received from the core since load; nonsilent_out = 1 if any non-near-zero sample was seen. */
void rp_runtime_audio_stats(rp_runtime* rt, uint64_t* frames_out, int* nonsilent_out);

/* Size in bytes of the loaded core's savestate, or 0 if no core is loaded / the core does not
   support serialization. */
size_t rp_runtime_serialize_size(rp_runtime* rt);

/* Write the loaded core's current state into buf (must be >= rp_runtime_serialize_size(rt)
   bytes). RP_ERR_UNSUPPORTED if the core has no savestate; RP_ERR_BAD_ARG on null args or an
   undersized buffer. */
rp_result rp_runtime_save_state(rp_runtime* rt, void* buf, size_t size);

/* Restore the loaded core's state from buf/size (as previously written by save_state). A core
   rejecting an incompatible state surfaces as an error result, not a crash. */
rp_result rp_runtime_load_state(rp_runtime* rt, const void* buf, size_t size);

#ifdef __cplusplus
}
#endif
#endif /* RETROPARK_H */
