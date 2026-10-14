#version 450
#extension GL_GOOGLE_include_directive : require
#include "globals.glsl"
layout(location=0) in vec3 worldPosition;
layout(location=1) in vec3 worldNormal;
layout(location=2) in vec3 surfaceColor;
layout(location=3) noperspective in vec3 barycentric;
layout(location=0) out vec4 outColor;
layout(push_constant) uniform Push { mat4 viewProjection; vec4 selected; vec4 chunkOffset; vec4 fineTarget; vec4 viewport; } pc;
void main() {
    vec3 cell=floor(worldPosition-worldNormal*0.000008);
    float ambient=pc.chunkOffset.w>0.5?0.16:0.42;
    vec3 sun=pc.chunkOffset.w>0.5?pc.selected.xyz:normalize(vec3(-0.45,0.85,0.35));
    float light=ambient+(1.0-ambient)*max(dot(normalize(worldNormal),sun),0.0);
    // Per-cell variation lives in shading so it does not prevent equal-material quads from merging.
    uvec3 c=uvec3(cell);
    uint noise=(c.x*73856093u)^(c.y*19349663u)^(c.z*83492791u);
    float variation=0.91+float(noise%100u)*0.0015;
    if(pc.chunkOffset.w>0.5) variation=1.0;
    vec3 baseColor=surfaceColor;
    int terrainSlot=int(round(-pc.selected.w))-1;
    if(pc.chunkOffset.w>0.5 && pc.chunkOffset.w<1.5 && g.cameraEarth.w>0.5)
        baseColor=patchColor(terrainSlot,g.cameraEarth.xyz+(worldPosition+pc.chunkOffset.xyz)*.125,
                             pc.viewport.w<0.0,surfaceColor);
    vec3 color=baseColor*variation*light;
    if(pc.chunkOffset.w>0.5 && pc.viewport.w>=0.0) {
        vec3 relative=(worldPosition+pc.chunkOffset.xyz)*0.125;
        vec3 stable=g.detailOrigin.xyz+relative;
        float detail=detailNoise(stable,4.0)*.10+detailNoise(stable,1.0)*.09+detailNoise(stable,.25)*.06+detailNoise(stable,.0625)*.025+detailNoise(stable,.016)*.015+detailNoise(stable,.004)*.01;
        vec3 albedo=baseColor*(1.0+detail);
        float distant=smoothstep(1500.0,15000.0,length(relative));
        if(pc.chunkOffset.w<1.5 && g.cameraEarth.w>0.5)
            albedo=mix(albedo,earthColor(g.cameraEarth.xyz+relative),distant);
        // Material bump lighting uses screen derivatives, no extra mesh vertices.
        vec3 dp1=dFdx(relative),dp2=dFdy(relative),n=normalize(worldNormal);
        vec3 tangent1=cross(dp2,n),tangent2=cross(n,dp1);
        float denominator=max(abs(dot(dp1,tangent1)),1e-8);
        vec3 gradient=(dFdx(detail)*tangent1+dFdy(detail)*tangent2)/denominator;
        n=normalize(n-clamp(gradient,vec3(-.3),vec3(.3))*.06);
        if(pc.chunkOffset.w<1.5) {
            vec3 ecef=g.cameraEarth.xyz+relative;
            vec3 ellipsoidNormal=normalize(ecef*vec3(1,1,1.0067394967422765));
            n=normalize(mix(n,ellipsoidNormal,distant));
        }
        light=ambient+(1.0-ambient)*max(dot(n,sun),0.0);
        color=albedo*light;
    }
    if(pc.chunkOffset.w>0.5 && pc.viewport.w<0.0) color=baseColor;
    vec3 f=fract(worldPosition);
    vec2 faceUV=abs(worldNormal.x)>0.5?f.yz:(abs(worldNormal.y)>0.5?f.xz:f.xy);
    float edge=min(min(faceUV.x,1.0-faceUV.x),min(faceUV.y,1.0-faceUV.y));
    if(pc.chunkOffset.w<0.5) color*=mix(0.84,1.0,smoothstep(0.0,0.018,edge));
    bool targeted=pc.selected.w>0.5 && all(equal(cell,pc.selected.xyz));
    if(pc.fineTarget.w>0.5) {
        vec3 millimeter=worldPosition*125.0;
        vec3 mmCell=floor(millimeter-worldNormal*0.001);
        targeted=all(equal(mmCell,pc.fineTarget.xyz));
        vec3 mf=fract(millimeter);
        vec2 uv=abs(worldNormal.x)>0.5?mf.yz:(abs(worldNormal.y)>0.5?mf.xz:mf.xy);
        edge=min(min(uv.x,1.0-uv.x),min(uv.y,1.0-uv.y));
    }
    if(targeted)
        color=mix(color,vec3(1.0,0.85,0.39),edge<0.035?0.95:0.16);
    // Fog uses physical distance so changing cell resolution does not change its appearance.
    float fog=1.0-exp(-0.00000035*pc.viewport.w*pc.viewport.w/
                      (gl_FragCoord.w*gl_FragCoord.w));
    if(pc.chunkOffset.w>0.5) fog=0.0;
    color=mix(color,vec3(0.48,0.67,0.82),clamp(fog,0.0,0.7));
    // Shader wireframe reveals actual triangle edges without requiring fillModeNonSolid.
    if(pc.viewport.z>0.5) {
        vec3 edgeBlend=smoothstep(vec3(0),max(fwidth(barycentric),vec3(0.000001))*1.2,barycentric);
        float triangleEdge=1.0-min(min(edgeBlend.x,edgeBlend.y),edgeBlend.z);
        if(pc.viewport.z>1.5) {
            if(triangleEdge<0.15) discard;
            vec3 lineColor=targeted?vec3(1.0,0.65,0.15):vec3(0.30,0.90,0.68);
            color=mix(vec3(0.035,0.075,0.085),lineColor,triangleEdge);
        } else {
            color=mix(color,vec3(0.025,0.045,0.065),triangleEdge*0.9);
        }
    }
    outColor=vec4(color,1.0);
}
