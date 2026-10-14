#ifndef MATH3D_H
#define MATH3D_H
#include <math.h>
typedef struct {
    float x, y, z;
} Vec3;
typedef struct {
    float m[16];
} Mat4;
static inline Vec3 v3(float x, float y, float z) {
    return (Vec3){x, y, z};
}
static inline Vec3 vadd(Vec3 a, Vec3 b) {
    return v3(a.x + b.x, a.y + b.y, a.z + b.z);
}
static inline Vec3 vsub(Vec3 a, Vec3 b) {
    return v3(a.x - b.x, a.y - b.y, a.z - b.z);
}
static inline Vec3 vscale(Vec3 a, float s) {
    return v3(a.x * s, a.y * s, a.z * s);
}
static inline float vdot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static inline Vec3 vcross(Vec3 a, Vec3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline Vec3 vnormalize(Vec3 a) {
    float n = sqrtf(vdot(a, a));
    return n > 0 ? vscale(a, 1 / n) : v3(0, 0, 0);
}
Mat4 mat_mul(Mat4 a, Mat4 b);
Mat4 mat_perspective(float fov, float aspect, float near_z, float far_z);
Mat4 mat_perspective_reverse_z(float fov, float aspect, float near_z, float far_z);
Mat4 mat_view(Vec3 position, Vec3 direction);
Mat4 mat_view_up(Vec3 position, Vec3 direction, Vec3 up);
#endif
