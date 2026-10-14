layout(set=0,binding=0,std140) uniform Globals {
    vec4 cameraEarth;
    vec4 detailOrigin;
    vec4 sunTime;
    vec4 absorption;
    vec4 scattering;
    vec4 bodyDirectionRadius[9];
    vec4 bodyLightDistance[9];
    vec4 bodyColor[9];
    vec4 materialColor[11];
    vec4 planetAxes;
} g;
layout(set=0,binding=1) uniform sampler2D terrainAtlas;
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
    int material=clamp(int(round(texelFetch(terrainAtlas,pixel,0).b*255.0)),1,10);
    return mix(g.materialColor[9].rgb,g.materialColor[material].rgb,land);
}
float detailNoise(vec3 p,float scale) {
    float footprint=max(length(dFdx(p)),length(dFdy(p)));
    float weight=1.0-smoothstep(scale*.18,scale*.7,footprint);
    if(weight<.001)return 0.0;
    return sin(p.x*2.0*PI/scale)*sin(p.y*2.0*PI/scale)*sin(p.z*2.0*PI/scale)*weight;
}
