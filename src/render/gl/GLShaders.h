#pragma once
namespace rp {
inline const char* kGLVertSrc =
  "#version 330 core\n"
  "out vec2 vUV;\n"
  "uniform int uFlipV;\n"
  "uniform vec2 uUVScale;\n"                                // (1,1) normally; (w/texW,h/texH) to sample a sub-region
  "void main(){\n"
  "  vec2 p = vec2((gl_VertexID<<1)&2, gl_VertexID&2);\n"   // (0,0)(2,0)(0,2) fullscreen tri
  "  vec2 base = vec2(p.x, uFlipV != 0 ? 1.0 - p.y : p.y);\n" // flipV: top-origin data flips V (row 0 is the top)
  "  vUV = base * uUVScale;\n"                              // scale UVs to the valid w*h sub-region of the texture
  "  gl_Position = vec4(p*2.0-1.0, 0.0, 1.0);\n"
  "}\n";
inline const char* kGLFragSrc =
  "#version 330 core\n"
  "in vec2 vUV; out vec4 oColor; uniform sampler2D uTex;\n"
  "void main(){ oColor = texture(uTex, vUV); }\n";
}
