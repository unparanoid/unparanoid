#version 330
#extension GL_ARB_explicit_uniform_location : enable


layout(location = 1) uniform vec2 u_scale;
layout(location = 2) uniform vec2 u_size;

out vec2 v_uv;


const vec2[] square_ = vec2[](
	vec2(-1.,  1.), vec2(-1., -1.), vec2( 1., -1.),
	vec2(-1.,  1.), vec2( 1., -1.), vec2( 1.,  1.)
);

const vec2[] qsquare_ = vec2[](
	vec2(0., 1.), vec2(0., 0.), vec2(1., 0.),
	vec2(0., 1.), vec2(1., 0.), vec2(1., 1.)
);


void main(void) {
	v_uv        = qsquare_[gl_VertexID]*u_size;
	gl_Position = vec4(square_[gl_VertexID]*u_scale, 0., 1.);
}
