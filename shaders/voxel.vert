#version 450
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec3 color;
layout(location=0) out vec3 worldPosition;
layout(location=1) out vec3 worldNormal;
layout(location=2) out vec3 surfaceColor;
layout(location=3) noperspective out vec3 barycentric;
layout(push_constant) uniform Push { mat4 viewProjection; vec4 selected; vec4 chunkOffset; vec4 fineTarget; vec4 viewport; } pc;
void main() {
    worldPosition=position; worldNormal=normal; surfaceColor=color;
    // Mesh draws use non-indexed triangles, with firstVertex=0 and three vertices per triangle.
    int corner=gl_VertexIndex%3;
    barycentric=corner==0?vec3(1,0,0):(corner==1?vec3(0,1,0):vec3(0,0,1));
    gl_Position=pc.viewProjection*vec4(position+pc.chunkOffset.xyz,1.0);
}
