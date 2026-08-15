#pragma once
#include "render/gl/GLContext.h"
#include <string>
namespace rp {
class GLCompositor {
public:
    bool initialize(const GLFns& g, std::string& err);
    void draw(const GLFns& g, GLuint tex);   // into the currently-bound framebuffer + viewport
    void destroy(const GLFns& g);
private:
    GLuint prog_ = 0, vao_ = 0; GLint uTex_ = -1;
};
}
