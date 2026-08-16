#pragma once
#include "render/gl/GLContext.h"
#include <string>
namespace rp {
class GLCompositor {
public:
    bool initialize(const GLFns& g, std::string& err);
    // into the currently-bound framebuffer + viewport. flipV=1 (default) flips V for top-origin CPU
    // frames; flipV=0 leaves V unflipped for a bottom-origin (GL) source texture.
    // uvScaleX/Y default to 1 (sample the whole texture). Pass (w/texW, h/texH) to sample only the
    // valid w*h sub-region of a larger max-geometry texture (a HW-render core that renders sub-max).
    void draw(const GLFns& g, GLuint tex, int flipV = 1, float uvScaleX = 1.0f, float uvScaleY = 1.0f);
    void destroy(const GLFns& g);
private:
    GLuint prog_ = 0, vao_ = 0; GLint uTex_ = -1; GLint uFlipV_ = -1; GLint uUVScale_ = -1;
};
}
