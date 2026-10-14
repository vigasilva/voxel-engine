#version 450
layout(location=0) in vec3 position;
layout(location=2) in vec3 color;
layout(location=0) out vec3 tint;
layout(push_constant) uniform Push { mat4 viewProjection; vec4 selected; vec4 chunkOffset; vec4 fineTarget; vec4 viewport; } pc;
void main() {
    gl_Position=vec4(position.x/pc.viewport.x*2.0-1.0,position.y/pc.viewport.y*2.0-1.0,0,1);
    tint=color;
}
