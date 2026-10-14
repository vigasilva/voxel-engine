#include "material_ids.glsl"
layout(set=0,binding=0,std140) uniform Globals {
    vec4 cameraEarth;
    vec4 detailOrigin;
    vec4 sunTime;
    vec4 absorption;
    vec4 scattering;
    vec4 bodyDirectionRadius[9];
    vec4 bodyLightDistance[9];
    vec4 bodyColor[9];
    vec4 materialColor[MATERIAL_COUNT];
    vec4 planetAxes;
    vec4 patchMaterial[768];
} g;
layout(set=0,binding=1) uniform sampler2D terrainAtlas;
layout(set=0,binding=2) uniform sampler2DArray terrainFields0;
layout(set=0,binding=3) uniform sampler2DArray terrainFields1;
layout(set=0,binding=4) uniform sampler2DArray terrainFields2;
// The negative selected.w encodes a cache slot without enlarging push constants.
vec3 patchColor(int slot, vec3 position, bool materialView, vec3 fallback) {
    if(slot<0 || slot>=768) return fallback;
    vec4 mapping=g.patchMaterial[slot];
    if(mapping.w<7.5) return fallback;
    int face=int(round(mapping.w))-8;
    // Project the elevated point back toward the reference ellipsoid normal.
    // This keeps the cube UV mapping close to the measured ground at elevation.
    vec3 axes=vec3(g.planetAxes.x,g.planetAxes.x,g.planetAxes.y);
    vec3 normal=normalize(position/(axes*axes));
    float radial=1.0/length(normalize(position)/axes);
    vec3 base=position-normal*(length(position)-radial);
    vec2 cube;
    if(face==0) cube=vec2(-base.z,base.y)/base.x;
    else if(face==1) cube=vec2(base.z,base.y)/(-base.x);
    else if(face==2) cube=vec2(base.x,-base.z)/base.y;
    else if(face==3) cube=vec2(base.x,base.z)/(-base.y);
    else if(face==4) cube=base.xy/base.z;
    else cube=vec2(-base.x,base.y)/(-base.z);
    vec2 uv=(cube-mapping.xy)*mapping.z;
    vec2 dx=dFdx(uv),dy=dFdy(uv);
    int group=slot/256,layer=slot%256;
    if(materialView) {
        ivec3 pixel=ivec3(clamp(uv,vec2(0),vec2(.999999))*256.0,layer);
        float id;
        if(group==0) id=texelFetch(terrainFields0,pixel,0).a;
        else if(group==1) id=texelFetch(terrainFields1,pixel,0).a;
        else id=texelFetch(terrainFields2,pixel,0).a;
        return g.materialColor[clamp(int(round(id*255.0)),0,MATERIAL_COUNT-1)].rgb;
    }
    if(group==0) return textureGrad(terrainFields0,vec3(uv,layer),dx,dy).rgb;
    if(group==1) return textureGrad(terrainFields1,vec3(uv,layer),dx,dy).rgb;
    return textureGrad(terrainFields2,vec3(uv,layer),dx,dy).rgb;
}
const float PI=3.14159265358979323846;
vec2 earthUV(vec3 position) {
    // Ellipsoid surface normal maps geocentric positions to geodetic latitude.
    vec3 n=normalize(position*vec3(1,1,1.0067394967422765));
    return vec2(atan(n.y,n.x)/(2.0*PI)+0.5,0.5-asin(clamp(n.z,-1.0,1.0))/PI);
}
vec3 earthColor(vec3 position) {
    vec2 uv=earthUV(position);
    vec2 dx=dFdx(uv),dy=dFdy(uv);
    dx.x=mod(dx.x+0.5,1.0)-0.5;dy.x=mod(dy.x+0.5,1.0)-0.5;
    vec4 terrain=textureGrad(terrainAtlas,uv,dx,dy);
    float height=(terrain.r*65280.0+terrain.g*255.0)/65535.0*24000.0-12000.0;
    // Filter signed height and classify AFTER filtering: coastlines aren't mesh/material blocks.
    float shore=max(.4,fwidth(height)*.65);
    float land=smoothstep(-shore,shore,height)*(1.0-smoothstep(.4,.6,terrain.a));
    ivec2 dimensions=textureSize(terrainAtlas,0);
    ivec2 pixel=ivec2(fract(uv.x)*dimensions.x,clamp(uv.y,0.0,.999999)*dimensions.y);
    int material=clamp(int(round(texelFetch(terrainAtlas,pixel,0).b*255.0)),1,MATERIAL_COUNT-1);
    return mix(g.materialColor[9].rgb,g.materialColor[material].rgb,land);
}
float detailNoise(vec3 p,float scale) {
    float footprint=max(length(dFdx(p)),length(dFdy(p)));
    float weight=1.0-smoothstep(scale*.18,scale*.7,footprint);
    if(weight<.001)return 0.0;
    return sin(p.x*2.0*PI/scale)*sin(p.y*2.0*PI/scale)*sin(p.z*2.0*PI/scale)*weight;
}
