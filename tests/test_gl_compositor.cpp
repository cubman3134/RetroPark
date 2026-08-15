#include <doctest/doctest.h>
#include "render/gl/GLContext.h"
#include "render/gl/GLCompositor.h"
#include <vector>
using namespace rp;
TEST_CASE("gl compositor: draws a texture into an FBO") {
    if (!GLContext::probe()) { WARN("no GL 3.3"); return; }
    std::string err; GLContext ctx; REQUIRE(ctx.initialize(nullptr,16,16,err)); REQUIRE(ctx.make_current());
    const GLFns& g = ctx.gl();
    GLCompositor comp; REQUIRE(comp.initialize(g, err));
    // A 2x2 solid magenta (255,0,255,255) source texture.
    GLuint tex=0; g.GenTextures(1,&tex); g.BindTexture(0x0DE1/*GL_TEXTURE_2D*/,tex);
    g.TexParameteri(0x0DE1,0x2801/*MIN_FILTER*/,0x2600/*NEAREST*/);
    g.TexParameteri(0x0DE1,0x2800/*MAG_FILTER*/,0x2600);
    unsigned char px[2*2*4]; for(int i=0;i<4;i++){px[i*4]=255;px[i*4+1]=0;px[i*4+2]=255;px[i*4+3]=255;}
    g.TexImage2D(0x0DE1,0,0x8058/*RGBA8*/,2,2,0,0x1908/*GL_RGBA*/,0x1401/*UNSIGNED_BYTE*/,px);
    // FBO target 16x16.
    GLuint fbo=0,dst=0; g.GenFramebuffers(1,&fbo); g.GenTextures(1,&dst);
    g.BindTexture(0x0DE1,dst); g.TexImage2D(0x0DE1,0,0x8058,16,16,0,0x1908,0x1401,nullptr);
    g.BindFramebuffer(0x8D40/*FRAMEBUFFER*/,fbo);
    g.FramebufferTexture2D(0x8D40,0x8CE0/*COLOR_ATTACHMENT0*/,0x0DE1,dst,0);
    REQUIRE(g.CheckFramebufferStatus(0x8D40)==0x8CD5/*FRAMEBUFFER_COMPLETE*/);
    g.Viewport(0,0,16,16);
    comp.draw(g, tex);
    std::vector<unsigned char> out(16*16*4,0);
    g.ReadPixels(0,0,16,16,0x1908,0x1401,out.data());
    CHECK(out[0]==255); CHECK(out[1]==0); CHECK(out[2]==255);   // magenta filled the FBO
}
