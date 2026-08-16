#pragma once
#include "render/gl/GLContext.h"
#include <string>
namespace rp {
class GLCompositor {
public:
    bool initialize(const GLFns& g, std::string& err);
    // into the currently-bound framebuffer + viewport. flipV=1 (default) flips V for top-origin CPU
    // frames; flipV=0 leaves V unflipped for a bottom-origin (GL) source texture.
    void draw(const GLFns& g, GLuint tex, int flipV = 1);
    void destroy(const GLFns& g);
private:
    GLuint prog_ = 0, vao_ = 0; GLint uTex_ = -1; GLint uFlipV_ = -1;
};
}
