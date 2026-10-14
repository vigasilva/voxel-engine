#include "math3d.h"
Mat4 mat_mul(Mat4 a, Mat4 b) {
    Mat4 r = {0};
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++)
            for (int k = 0; k < 4; k++)
                r.m[c * 4 + row] += a.m[k * 4 + row] * b.m[c * 4 + k];
    return r;
}
Mat4 mat_perspective(float fov, float aspect, float near_z, float far_z) {
    float f = 1 / tanf(fov / 2);
    Mat4 r = {0};
    r.m[0] = f / aspect;
    r.m[5] = -f; /* Vulkan Y-down clip space, depth 0..1. */
    r.m[10] = far_z / (near_z - far_z);
    r.m[11] = -1;
    r.m[14] = (far_z * near_z) / (near_z - far_z);
    return r;
}
Mat4 mat_perspective_reverse_z(float fov, float aspect, float near_z, float far_z) {
    Mat4 r = mat_perspective(fov, aspect, near_z, far_z);
    r.m[10] = near_z / (far_z - near_z);
    r.m[14] = (far_z * near_z) / (far_z - near_z);
    return r;
}
Mat4 mat_view(Vec3 p, Vec3 dir) {
    return mat_view_up(p, dir, v3(0, 1, 0));
}
Mat4 mat_view_up(Vec3 p, Vec3 dir, Vec3 up) {
    Vec3 f = vnormalize(dir), s = vnormalize(vcross(f, up));
    if (vdot(s, s) < .5f)
        s = vnormalize(vcross(f, fabsf(f.z) < .9f ? v3(0, 0, 1) : v3(0, 1, 0)));
    Vec3 u = vcross(s, f);
    return (Mat4){
        {s.x, u.x, -f.x, 0, s.y, u.y, -f.y, 0, s.z, u.z, -f.z, 0, -vdot(s, p), -vdot(u, p), vdot(f, p), 1}};
}
