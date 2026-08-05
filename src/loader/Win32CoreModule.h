#pragma once
#include <memory>
#include <string>
#include "loader/ICoreModule.h"
#include <retropark/retropark_abi.h>

namespace rp {
class Win32CoreModule : public ICoreModule {
public:
    static rp_result open(const std::string& dll_path,
                          std::unique_ptr<Win32CoreModule>& out, std::string& error);
    ~Win32CoreModule() override;
    void* resolve(const char* symbol) override;
private:
    explicit Win32CoreModule(void* handle) : handle_(handle) {}
    void* handle_ = nullptr;
};
}
