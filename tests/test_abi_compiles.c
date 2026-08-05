#include <retropark/retropark_abi.h>

/* Compile-time proof the ABI is C-clean and self-consistent. */
int retropark_abi_c_check(void) {
    rp_core_info info;
    info.abi_version = RETROPARK_ABI_VERSION;
    info.type = RP_CORE_PRESENTING;
    info.graphics_api = RP_GFX_D3D11;
    info.id = "x";
    return (int)info.abi_version;
}
