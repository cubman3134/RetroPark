#include "loader/Win32CoreModule.h"
#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

namespace rp {

rp_result Win32CoreModule::open(const std::string& path,
                                std::unique_ptr<Win32CoreModule>& out, std::string& error) {
#if defined(_WIN32)
    HMODULE h = ::LoadLibraryA(path.c_str());
    if (!h) { error = "LoadLibrary failed for " + path; return RP_ERR_NOT_FOUND; }
    out.reset(new Win32CoreModule(reinterpret_cast<void*>(h)));
    return RP_OK;
#else
    void* h = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) { error = dlerror() ? dlerror() : "dlopen failed"; return RP_ERR_NOT_FOUND; }
    out.reset(new Win32CoreModule(h));
    return RP_OK;
#endif
}

Win32CoreModule::~Win32CoreModule() {
    if (!handle_) return;
#if defined(_WIN32)
    ::FreeLibrary(reinterpret_cast<HMODULE>(handle_));
#else
    ::dlclose(handle_);
#endif
}

void* Win32CoreModule::resolve(const char* symbol) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(handle_), symbol));
#else
    return ::dlsym(handle_, symbol);
#endif
}
}
