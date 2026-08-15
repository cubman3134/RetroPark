#include "render/gl/GLCompositor.h"
#include "render/gl/GLShaders.h"
namespace rp {
static GLuint compile(const GLFns& g, GLenum type, const char* src, std::string& err) {
    GLuint s = g.CreateShader(type); g.ShaderSource(s,1,&src,nullptr); g.CompileShader(s);
    GLint ok=0; g.GetShaderiv(s,0x8B81/*COMPILE_STATUS*/,&ok);
    if(!ok){ char log[512]; GLsizei n=0; g.GetShaderInfoLog(s,512,&n,log); err.assign(log,n); return 0; }
    return s;
}
bool GLCompositor::initialize(const GLFns& g, std::string& err) {
    GLuint vs=compile(g,0x8B31/*VERTEX_SHADER*/,kGLVertSrc,err); if(!vs) return false;
    GLuint fs=compile(g,0x8B30/*FRAGMENT_SHADER*/,kGLFragSrc,err); if(!fs) return false;
    prog_=g.CreateProgram(); g.AttachShader(prog_,vs); g.AttachShader(prog_,fs); g.LinkProgram(prog_);
    GLint ok=0; g.GetProgramiv(prog_,0x8B82/*LINK_STATUS*/,&ok);
    g.DeleteShader(vs); g.DeleteShader(fs);
    if(!ok){ char log[512]; GLsizei n=0; g.GetProgramInfoLog(prog_,512,&n,log); err.assign(log,n); return false; }
    uTex_=g.GetUniformLocation(prog_,"uTex");
    g.GenVertexArrays(1,&vao_);
    return true;
}
void GLCompositor::draw(const GLFns& g, GLuint tex) {
    g.UseProgram(prog_); g.BindVertexArray(vao_);
    g.ActiveTexture(0x84C0/*TEXTURE0*/); g.BindTexture(0x0DE1/*TEXTURE_2D*/,tex);
    if(uTex_>=0) g.Uniform1i(uTex_,0);
    g.DrawArrays(0x0004/*GL_TRIANGLES*/,0,3);
}
void GLCompositor::destroy(const GLFns&) { /* program/vao freed with the context */ }
}
