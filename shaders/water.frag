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
    vec3 relative=(worldPosition+pc.chunkOffset.xyz)*.125;
    vec3 stable=g.detailOrigin.xyz+relative;
    vec3 normal=normalize(worldNormal),eye=normalize(-relative);
    float phase=g.sunTime.w*1.5;
    vec3 wave=vec3(cos(stable.x*PI+phase),cos(stable.y*PI*.5-phase*.5),cos(stable.z*PI+phase));
    normal=normalize(normal+(wave-normal*dot(wave,normal))*g.absorption.w);
    float cosine=clamp(dot(normal,eye),0.0,1.0);
    float ior=max(1.001,g.bodyColor[0].w), f0=pow((ior-1.0)/(ior+1.0),2.0);
    float fresnel=f0+(1.0-f0)*pow(1.0-cosine,5.0);
    // Representative optical path only; a future volume pass supplies actual ray depth.
    vec3 extinction=(g.absorption.xyz+vec3(g.scattering.w*.02))*max(.01,g.bodyColor[1].w/1000.0);
    vec3 transmission=exp(-extinction*2.0/max(.15,cosine));
    vec3 water=mix(g.scattering.xyz,surfaceColor,transmission);
    float light=.18+.82*max(dot(normal,pc.selected.xyz),0.0);
    vec3 reflection=mix(vec3(.12,.22,.33),vec3(.45,.64,.78),max(dot(normal,eye),0.0));
    float specular=pow(max(dot(reflect(-pc.selected.xyz,normal),eye),0.0),128.0);
    vec3 color=mix(water*light,reflection,fresnel)+vec3(1.0,.95,.78)*specular*.5;
    if(pc.chunkOffset.w<1.5&&g.cameraEarth.w>0.5)
        color=mix(color,earthColor(g.cameraEarth.xyz+relative)*light,smoothstep(1500.0,15000.0,length(relative)));
    if(pc.viewport.w<0.0)color=surfaceColor;
    if(pc.fineTarget.w>0.5) {
        vec3 mm=floor(worldPosition*125.0-worldNormal*.001);
        if(all(equal(mm,pc.fineTarget.xyz)))color=mix(color,vec3(1,.85,.39),.2);
    }
    if(pc.viewport.z>0.5) {
        vec3 edges=smoothstep(vec3(0),max(fwidth(barycentric),vec3(.000001))*1.2,barycentric);
        float edge=1.0-min(min(edges.x,edges.y),edges.z);
        if(pc.viewport.z>1.5&&edge<.15)discard;
        color=mix(color,pc.viewport.z>1.5?vec3(.3,.9,.68):vec3(.025,.045,.065),edge*.9);
    }
    outColor=vec4(color,1);
}
