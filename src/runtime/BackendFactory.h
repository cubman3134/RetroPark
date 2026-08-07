#pragma once
#include <memory>
#include "render/IRenderBackend.h"
#include <retropark/retropark_abi.h>
namespace rp { std::unique_ptr<IRenderBackend> make_backend(rp_graphics_api api); }
