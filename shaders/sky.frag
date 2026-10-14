#version 450
#extension GL_GOOGLE_include_directive : require
#include "globals.glsl"
layout(location=0) in vec2 screen;
layout(location=0) out vec4 outColor;
layout(push_constant) uniform Push { mat4 viewProjection; vec4 selected; vec4 chunkOffset; vec4 fineTarget; vec4 viewport; } pc;
void main() {
    vec3 row0=vec3(pc.viewProjection[0][0],pc.viewProjection[1][0],pc.viewProjection[2][0]);
    vec3 row1=vec3(pc.viewProjection[0][1],pc.viewProjection[1][1],pc.viewProjection[2][1]);
    vec3 forward=vec3(pc.viewProjection[0][3],pc.viewProjection[1][3],pc.viewProjection[2][3]);
    vec3 ray=normalize(forward+screen.x*row0/dot(row0,row0)+screen.y*row1/dot(row1,row1));
    vec3 sun=normalize(pc.selected.xyz),up=normalize(pc.chunkOffset.xyz);
    float daylight=smoothstep(-.15,.1,dot(up,sun));
    float horizon=pow(1.0-clamp(dot(ray,up),0.0,1.0),3.0);
    vec3 color=mix(vec3(.12,.32,.60),vec3(.63,.75,.83),horizon);
    color=mix(vec3(.005,.008,.02),color*mix(.025,1.0,daylight),pc.chunkOffset.w);
    float cosine=dot(ray,sun),glow=exp(-max(0.0,1.0-cosine)*2800.0)*pc.chunkOffset.w;
    color+=vec3(.20,.12,.04)*glow;
    float nearest=1e20,backgroundDepth=0.0;
    for(int i=0;i<9;i++) {
        vec3 dir=g.bodyDirectionRadius[i].xyz;
        float radius=g.bodyDirectionRadius[i].w;
        if(radius<=0.0||dot(ray,dir)<=0.0)continue;
        // Angular intersection retains precision even for small planets across AU distances.
        float side=length(cross(ray,dir));
        if(side>radius)continue;
        float distance=g.bodyLightDistance[i].w;
        if(distance>=nearest)continue;
        vec3 normal=normalize((ray-dir*dot(ray,dir))/radius-dir*sqrt(max(0.0,1.0-side*side/(radius*radius))));
        vec3 albedo=g.bodyColor[i].rgb;
        float bodyDepth=0.0;
        if(i==3) {
            if(pc.viewport.z>1.5&&g.detailOrigin.w<.5)continue;
            // Complete oblate Earth background while visible mesh jobs stream in.
            // Project to the closest point first to avoid subtracting large quadratic terms.
            vec3 inverseAxes=1.0/vec3(g.planetAxes.x,g.planetAxes.x,g.planetAxes.y);
            vec3 origin=g.cameraEarth.xyz*inverseAxes,direction=ray*inverseAxes;
            float along=-dot(origin,direction)/dot(direction,direction);
            vec3 closest=origin+direction*along;
            float separation=dot(closest,closest);
            if(separation>1.0||along<=0.0)continue;
            float t=along-sqrt(max(0.0,1.0-separation)/dot(direction,direction));
            vec3 surface=g.cameraEarth.xyz+ray*t;
            normal=normalize(surface*inverseAxes*inverseAxes);
            if(g.cameraEarth.w>0.5)albedo=earthColor(surface);
            if(pc.viewport.z<.5) {
                vec4 clip=pc.viewProjection*vec4(ray*t*8.0,1.0);
                bodyDepth=clamp(clip.z/clip.w,0.0,1.0);
            }
        }
        if(i==5||i==6) {
            float bands=sin(normal.z*45.0+sin(normal.x*12.0)*.7);
            albedo*=.85+.15*bands;
            if(i==5)albedo=mix(albedo,vec3(.52,.24,.14),.25*exp(-80.0*pow(normal.z+.25,2.0))*smoothstep(.5,.9,normal.x));
        }
        if(i==1||i==4)albedo*=.88+.12*sin(normal.x*45.0)*sin(normal.y*35.0)*sin(normal.z*39.0);
        nearest=distance;
        backgroundDepth=bodyDepth;
        float ambient=i==3?(albedo.r<.1?.18:.16):.04;
        color=i==0?albedo:albedo*(ambient+(1.0-ambient)*max(dot(normal,g.bodyLightDistance[i].xyz),0.0));
    }
    // Saturn's ring plane is an inexpensive analytic annulus at the planet's real scale.
    vec3 ringDir=g.bodyDirectionRadius[6].xyz;
    float ringRadius=g.bodyDirectionRadius[6].w;
    vec3 ringAxis=normalize(vec3(.15,.43,.89));
    float denominator=dot(ray,ringAxis);
    if(ringRadius>.00001&&abs(denominator)>.00001) {
        vec3 center=ringDir/ringRadius;
        float t=dot(center,ringAxis)/denominator;
        vec3 offset=ray*t-center;
        float ringDistance=t*ringRadius*g.bodyLightDistance[6].w;
        float radius=length(offset);
        if(t>0.0&&radius>1.25&&radius<2.35&&ringDistance<nearest) {
            float band=.7+.3*sin(radius*95.0);
            float gap=1.0-smoothstep(.012,.025,abs(radius-2.02));
            if(gap<.5) color=vec3(.62,.55,.42)*band*(.18+.82*abs(dot(ringAxis,g.bodyLightDistance[6].xyz)));
        }
    }
    gl_FragDepth=backgroundDepth;
    outColor=vec4(color,1);
}
