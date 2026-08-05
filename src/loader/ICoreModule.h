#pragma once
namespace rp {
struct ICoreModule {
    virtual ~ICoreModule() = default;
    virtual void* resolve(const char* symbol) = 0;
};
}
