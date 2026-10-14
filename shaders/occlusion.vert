#version 450
layout(location=0) in vec3 position;
layout(push_constant) uniform Push { mat4 viewProjection; vec4 selected; vec4 chunkOffset; vec4 fineTarget; vec4 viewport; } pc;
void main() { gl_Position=pc.viewProjection*vec4(position,1); }
