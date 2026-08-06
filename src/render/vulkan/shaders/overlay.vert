#version 450
layout(push_constant) uniform P { vec4 rect; vec4 color; } pc;
void main(){ vec2 c[4]=vec2[4](pc.rect.xy,pc.rect.zy,pc.rect.xw,pc.rect.zw); gl_Position=vec4(c[gl_VertexIndex],0,1); }
