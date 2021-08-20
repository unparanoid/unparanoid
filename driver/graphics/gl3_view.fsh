#version 330
#extension GL_ARB_explicit_uniform_location : enable


layout(location = 0) uniform sampler2D u_tex;

in  vec2 v_uv;
out vec4 o_color;

void main(void) {
	o_color = texture(u_tex, v_uv);
}
