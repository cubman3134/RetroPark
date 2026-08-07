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

/* Enable/disable the frame-by-frame rewind ring. Requires a serialize-capable driven core
   (RP_ERR_UNSUPPORTED otherwise); RP_ERR_BAD_ARG on null rt. When enabled, each forward
   rp_runtime_present captures the core's pre-frame state into a bounded ring of at most
   max_snapshots entries (a sane default is used when max_snapshots is 0). Toggling clears the
   ring. */
rp_result rp_runtime_set_rewind(rp_runtime* rt, int enabled, uint32_t max_snapshots);

/* Step the game one frame into the past: discard the newest snapshot and restore the previous
   frame's pre-state, so the next rp_runtime_present re-renders that earlier frame (and does not
   re-grow the ring). RP_ERR_BAD_ARG on null rt; RP_ERR_INTERNAL if rewind is disabled;
   RP_ERR_NOT_FOUND when there is no history left to step back to. */
rp_result rp_runtime_rewind(rp_runtime* rt);

#ifdef __cplusplus
}
#endif
#endif /* RETROPARK_H */
