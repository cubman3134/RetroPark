#pragma once
#include <cstring>
#include "loader/ICoreModule.h"
#include <retropark/retropark_abi.h>

namespace rp {
// ICoreModule backed by a compiled-in core getter (no dlopen). The static twin of Win32CoreModule: the
// core's code lives in the app binary, so there is nothing to load or free.
class StaticCoreModule : public ICoreModule {
public:
    explicit StaticCoreModule(rp_get_core_abi_fn getter) : getter_(getter) {}
    void* resolve(const char* symbol) override {
        if (symbol && std::strcmp(symbol, RP_CORE_ABI_EXPORT_NAME) == 0)
            return reinterpret_cast<void*>(getter_);
        return nullptr;
    }
private:
    rp_get_core_abi_fn getter_ = nullptr;
};
}
