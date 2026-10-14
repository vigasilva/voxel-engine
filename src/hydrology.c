#define _DEFAULT_SOURCE
#include "hydrology.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <zlib.h>
#ifdef __unix__
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#define HEAD_BINS 32
_Static_assert(sizeof(HydroBody) == 184, "Hydrology cache body layout");
static int compare_float(const void *aa, const void *bb) {
    float a = *(const float *)aa, b = *(const float *)bb;
    return a < b ? -1 : a > b;
}
static void nearest_pass(size_t *nearest, size_t w, size_t h, double dx, double dy, bool reverse) {
    size_t count = w * h;
    for (size_t step = 0; step < count; step++) {
        size_t i = reverse ? count - 1 - step : step;
        size_t x = i % w, y = i / w;
        int direction = reverse ? 1 : -1;
        const int offsets[4][2] = {{1, 0}, {0, 1}, {-1, 1}, {1, 1}};
        for (int k = 0; k < 4; k++) {
            ptrdiff_t xx = (ptrdiff_t)x + offsets[k][0] * direction,
                      yy = (ptrdiff_t)y + offsets[k][1] * direction;
            if (xx < 0 || yy < 0 || xx >= (ptrdiff_t)w || yy >= (ptrdiff_t)h)
                continue;
            size_t candidate = nearest[(size_t)yy * w + (size_t)xx];
            if (candidate == SIZE_MAX)
                continue;
            double cx = ((double)x - candidate % w) * dx, cy = ((double)y - candidate / w) * dy;
            size_t current = nearest[i];
            double ax = current == SIZE_MAX ? 1e30 : ((double)x - current % w) * dx,
                   ay = current == SIZE_MAX ? 1e30 : ((double)y - current / w) * dy;
            if (cx * cx + cy * cy < ax * ax + ay * ay)
                nearest[i] = candidate;
        }
    }
}
static double axis_at(size_t i, size_t w, double dx, double dy, double mx, double my, double ax, double ay) {
    return (i % w * dx - mx) * ax + (i / w * dy - my) * ay;
}
static double smooth(double x) {
    x = fmax(0, fmin(1, x));
    return x * x * (3 - 2 * x);
}
double hydrology_bed(double ground, double level, double shore, double depth, double bank) {
    if (!isfinite(level) || !isfinite(shore) || depth <= 0 || bank <= 0)
        return ground;
    if (shore >= 0)
        return level - depth * smooth(shore / bank);
    return level + (ground - level) * smooth(-shore / bank);
}
void hydrology_destroy(Hydrology *h) {
    if (!h)
        return;
    if (h->storage) {
#ifdef __unix__
        if (h->mapped)
            munmap(h->storage, h->storage_bytes);
        else
#endif
            free(h->storage);
    } else {
        free(h->body_ids);
        free(h->bodies);
        free(h->shore_m);
    }
    *h = (Hydrology){0};
}
static void put32(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}
static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put_double(unsigned char *p, double v) {
    uint64_t bits;
    memcpy(&bits, &v, 8);
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)(bits >> (8 * i));
}
static double get_double(const unsigned char *p) {
    uint64_t bits = 0;
    for (int i = 0; i < 8; i++)
        bits |= (uint64_t)p[i] << (8 * i);
    double value;
    memcpy(&value, &bits, 8);
    return value;
}
bool hydrology_load(Hydrology *out, const char *path, uint32_t source) {
    uint16_t endian = 1;
    if (!out || *(unsigned char *)&endian != 1)
        return false; /* Big-endian hosts build an in-memory profile instead. */
    Hydrology result = {0};
#ifdef __unix__
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0)
        return false;
    if (fstat(fd, &st) == 0 && st.st_size >= 64 && (uint64_t)st.st_size <= UINT_MAX) {
        result.storage_bytes = (size_t)st.st_size;
        void *p = mmap(NULL, result.storage_bytes, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p != MAP_FAILED) {
            result.storage = p;
            result.mapped = true;
        }
    }
    close(fd);
#else
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    if (!fseek(f, 0, SEEK_END)) {
        long bytes = ftell(f);
        if (bytes >= 64 && (uint64_t)bytes <= UINT_MAX) {
            result.storage_bytes = (size_t)bytes;
            result.storage = malloc(result.storage_bytes);
        }
    }
    rewind(f);
    if (result.storage && fread(result.storage, 1, result.storage_bytes, f) != result.storage_bytes) {
        free(result.storage);
        result.storage = NULL;
    }
    fclose(f);
#endif
    if (!result.storage)
        return false;
    unsigned char *p = result.storage;
    result.width = get32(p + 8);
    result.height = get32(p + 12);
    result.body_count = result.body_capacity = get32(p + 16);
    result.pixel_x_m = get_double(p + 32);
    result.pixel_y_m = get_double(p + 40);
    uint64_t cells = (uint64_t)result.width * result.height;
    bool ok = !memcmp(p, "HYDR", 4) && get32(p + 4) == 1 && get32(p + 20) == source && result.width >= 2 &&
              result.height >= 2 && cells <= (result.storage_bytes - 64) / 8 &&
              64 + cells * 8 + result.body_count * 184 == result.storage_bytes &&
              isfinite(result.pixel_x_m) && isfinite(result.pixel_y_m) && result.pixel_x_m > 0 &&
              result.pixel_y_m > 0 && get32(p + 28) == 0 && get_double(p + 48) == 0 &&
              get_double(p + 56) == 0 && crc32(0, p + 64, (uInt)(result.storage_bytes - 64)) == get32(p + 24);
    if (!ok) {
        hydrology_destroy(&result);
        return false;
    }
    result.body_ids = (uint32_t *)(p + 64);
    result.shore_m = (float *)(p + 64 + cells * 4);
    result.bodies = (HydroBody *)(p + 64 + cells * 8);
    for (size_t i = 0; i < cells; i++)
        if (result.body_ids[i] > result.body_count || !isfinite(result.shore_m[i])) {
            hydrology_destroy(&result);
            return false;
        }
    for (size_t i = 0; i < result.body_count; i++) {
        const HydroBody *b = &result.bodies[i];
        if (!isfinite(b->center_x_m) || !isfinite(b->center_y_m) || !isfinite(b->axis_x) ||
            !isfinite(b->axis_y) || !isfinite(b->minimum_m) || !isfinite(b->span_m) || b->span_m < 0 ||
            b->flags > 3 || b->reserved) {
            hydrology_destroy(&result);
            return false;
        }
        for (int j = 0; j < HEAD_BINS; j++)
            if (!isfinite(b->heads_m[j])) {
                hydrology_destroy(&result);
                return false;
            }
    }
#ifdef __unix__
    madvise(result.storage, result.storage_bytes, MADV_RANDOM);
    madvise(result.storage, result.storage_bytes, MADV_DONTNEED);
#endif
    *out = result;
    return true;
}
bool hydrology_save(const Hydrology *h, const char *path, uint32_t source) {
    if (!h || !h->body_ids || !h->shore_m || h->width > UINT32_MAX || h->height > UINT32_MAX ||
        h->body_count > UINT32_MAX)
        return false;
    uint64_t count = (uint64_t)h->width * h->height;
    if (count > (UINT_MAX - 64u) / 8 || h->body_count > (UINT_MAX - 64u - count * 8) / 184)
        return false;
    uint64_t size = 64 + count * 8 + h->body_count * 184;
    if (size > UINT_MAX || size > SIZE_MAX)
        return false;
    unsigned char *data = calloc((size_t)size, 1);
    if (!data)
        return false;
    memcpy(data, "HYDR", 4);
    put32(data + 4, 1);
    put32(data + 8, (uint32_t)h->width);
    put32(data + 12, (uint32_t)h->height);
    put32(data + 16, (uint32_t)h->body_count);
    put32(data + 20, source);
    put_double(data + 32, h->pixel_x_m);
    put_double(data + 40, h->pixel_y_m);
    for (size_t i = 0; i < count; i++) {
        uint32_t bits;
        put32(data + 64 + i * 4, h->body_ids[i]);
        memcpy(&bits, &h->shore_m[i], 4);
        put32(data + 64 + count * 4 + i * 4, bits);
    }
    for (size_t i = 0; i < h->body_count; i++) {
        const HydroBody *b = &h->bodies[i];
        unsigned char *p = data + 64 + count * 8 + i * 184;
        const double numbers[6] = {b->center_x_m, b->center_y_m, b->axis_x,
                                   b->axis_y,     b->minimum_m,  b->span_m};
        for (int j = 0; j < 6; j++)
            put_double(p + j * 8, numbers[j]);
        put32(p + 176, b->flags);
        for (int j = 0; j < HEAD_BINS; j++) {
            uint32_t bits;
            memcpy(&bits, &b->heads_m[j], 4);
            put32(p + 48 + j * 4, bits);
        }
    }
    put32(data + 24, (uint32_t)crc32(0, data + 64, (uInt)(size - 64)));
    char temporary[2048];
    FILE *f;
#ifdef __unix__
    snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
    int fd = mkstemp(temporary);
    f = fd >= 0 ? fdopen(fd, "wb") : NULL;
    if (fd >= 0 && !f) {
        close(fd);
        remove(temporary);
    }
#else
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    f = fopen(temporary, "wb");
#endif
    bool ok = false;
    if (f) {
        ok = fwrite(data, 1, (size_t)size, f) == size;
        if (fclose(f))
            ok = false;
        if (ok)
            ok = rename(temporary, path) == 0;
        if (!ok)
            remove(temporary);
    }
    free(data);
    return ok;
}
double hydrology_sample(const Hydrology *h, double x, double y, double *shore, bool *marine) {
    if (!h || !h->body_ids || !isfinite(x) || !isfinite(y))
        return NAN;
    size_t ix = (size_t)fmax(0, fmin(h->width - 1, floor(x + .5))),
           iy = (size_t)fmax(0, fmin(h->height - 1, floor(y + .5)));
    uint32_t id = h->body_ids[iy * h->width + ix];
    if (!id || id > h->body_count)
        return NAN;
    if (shore) {
        double cx = fmax(0, fmin(h->width - 1, x)), cy = fmax(0, fmin(h->height - 1, y));
        size_t x0 = (size_t)cx, y0 = (size_t)cy, x1 = x0 + (x0 + 1 < h->width),
               y1 = y0 + (y0 + 1 < h->height);
        double tx = cx - x0, ty = cy - y0;
        *shore =
            (h->shore_m[y0 * h->width + x0] * (1 - tx) + h->shore_m[y0 * h->width + x1] * tx) * (1 - ty) +
            (h->shore_m[y1 * h->width + x0] * (1 - tx) + h->shore_m[y1 * h->width + x1] * tx) * ty;
    }
    const HydroBody *body = &h->bodies[id - 1];
    if (marine)
        *marine = (body->flags & 2) != 0;
    if (body->span_m <= 0)
        return body->heads_m[0];
    double position = (x * h->pixel_x_m - body->center_x_m) * body->axis_x +
                      (y * h->pixel_y_m - body->center_y_m) * body->axis_y;
    double t = fmax(0, fmin(HEAD_BINS - 1, (position - body->minimum_m) / body->span_m * (HEAD_BINS - 1)));
    int bin = (int)fmin(HEAD_BINS - 2, floor(t));
    return body->heads_m[bin] + (body->heads_m[bin + 1] - body->heads_m[bin]) * (t - bin);
}
double hydrology_level(const Hydrology *h, double x, double y) {
    return hydrology_sample(h, x, y, NULL, NULL);
}
static bool append_body(Hydrology *h, HydroBody body, const size_t *pixels, size_t count) {
    if (h->body_count >= UINT32_MAX - 1)
        return false;
    if (h->body_count == h->body_capacity) {
        size_t capacity = h->body_capacity ? h->body_capacity * 2 : 16;
        if (capacity > SIZE_MAX / sizeof(HydroBody))
            return false;
        HydroBody *bodies = realloc(h->bodies, capacity * sizeof(*bodies));
        if (!bodies)
            return false;
        h->bodies = bodies;
        h->body_capacity = capacity;
    }
    h->bodies[h->body_count++] = body;
    for (size_t i = 0; i < count; i++)
        h->body_ids[pixels[i]] = (uint32_t)h->body_count;
    return true;
}
bool hydrology_build(Hydrology *out, const float *elevation, const uint8_t *water, size_t w, size_t h,
                     double dx, double dy) {
    if (!out || !elevation || !water || w < 2 || h < 2 || w > SIZE_MAX / h ||
        w * h > SIZE_MAX / sizeof(size_t) || !isfinite(dx) || !isfinite(dy) || dx <= 0 || dy <= 0)
        return false;
    size_t count = w * h;
    Hydrology result = {.width = w, .height = h, .pixel_x_m = dx, .pixel_y_m = dy};
    result.body_ids = calloc(count, sizeof(uint32_t));
    result.shore_m = malloc(count * sizeof(float));
    size_t *nearest = malloc(count * sizeof(size_t)), *queue = malloc(count * sizeof(size_t));
    uint8_t *visited = calloc(count, 1);
    float *samples = malloc(count * sizeof(float));
    if (!result.body_ids || !result.shore_m || !nearest || !queue || !visited || !samples)
        goto failed;
    for (size_t i = 0; i < count; i++) {
        nearest[i] = water[i] ? SIZE_MAX : i;
    }
    nearest_pass(nearest, w, h, dx, dy, false);
    nearest_pass(nearest, w, h, dx, dy, true);
    double half = fmin(dx, dy) * .5;
    for (size_t i = 0; i < count; i++) {
        double x = nearest[i] == SIZE_MAX ? 1e6 : ((double)(i % w) - nearest[i] % w) * dx,
               y = nearest[i] == SIZE_MAX ? 1e6 : ((double)(i / w) - nearest[i] / w) * dy;
        result.shore_m[i] = water[i] ? (float)fmax(0, hypot(x, y) - half) : 0;
    }
    for (size_t seed = 0; seed < count; seed++) {
        if (!water[seed] || visited[seed])
            continue;
        size_t begin = 0, end = 1, observed = 0;
        queue[0] = seed;
        visited[seed] = 1;
        double mx = 0, my = 0;
        bool boundary = false;
        while (begin < end) {
            size_t i = queue[begin++], x = i % w, y = i / w;
            boundary |= !x || !y || x + 1 == w || y + 1 == h;
            mx += x * dx;
            my += y * dy;
            if (isfinite(elevation[i]))
                samples[observed++] = elevation[i];
            const ptrdiff_t neighbours[4] = {-1, 1, -(ptrdiff_t)w, (ptrdiff_t)w};
            for (int k = 0; k < 4; k++) {
                if ((k == 0 && !x) || (k == 1 && x + 1 == w) || (k == 2 && !y) || (k == 3 && y + 1 == h))
                    continue;
                size_t n = (size_t)((ptrdiff_t)i + neighbours[k]);
                if (water[n] && !visited[n]) {
                    visited[n] = 1;
                    queue[end++] = n;
                }
            }
        }
        if (!observed)
            continue;
        qsort(samples, observed, sizeof(float), compare_float);
        double low = samples[observed / 10], high = samples[observed * 9 / 10];
        mx /= end;
        my /= end;
        double xx = 0, xy = 0, yy = 0;
        for (size_t j = 0; j < end; j++) {
            size_t i = queue[j];
            double x = i % w * dx - mx, y = i / w * dy - my;
            xx += x * x;
            xy += x * y;
            yy += y * y;
        }
        double angle = .5 * atan2(2 * xy, xx - yy), ax = cos(angle), ay = sin(angle),
               eigen = hypot(xx - yy, 2 * xy), major = (xx + yy + eigen) * .5,
               minor = fmax(1e-12, (xx + yy - eigen) * .5);
        bool river = major > minor * 9 && high - low > 2;
        /* Broad, nearly level bodies are lakes. Robust interior observations
           exclude the land elevations bilinearly mixed into water banks. */
        size_t core_count = 0;
        for (size_t j = 0; j < end; j++) {
            size_t i = queue[j], x = i % w, y = i / w;
            if (!isfinite(elevation[i]))
                continue;
            float local = result.shore_m[i];
            for (int ky = -2; ky <= 2; ky++)
                for (int kx = -2; kx <= 2; kx++) {
                    ptrdiff_t nx = (ptrdiff_t)x + kx, ny = (ptrdiff_t)y + ky;
                    if (nx >= 0 && ny >= 0 && nx < (ptrdiff_t)w && ny < (ptrdiff_t)h)
                        local = fmaxf(local, result.shore_m[(size_t)ny * w + (size_t)nx]);
                }
            if (result.shore_m[i] + half >= local)
                samples[core_count++] = elevation[i];
        }
        if (!core_count) {
            for (size_t j = 0; j < end; j++)
                if (isfinite(elevation[queue[j]]))
                    samples[core_count++] = elevation[queue[j]];
        }
        qsort(samples, core_count, sizeof(float), compare_float);
        double flat = samples[core_count / 2];
        /* Local masks lack ocean connectivity metadata. Only broad, nearly
           zero-height components crossing a tile edge are marine candidates. */
        bool marine = boundary && end * dx * dy > 1e6 && minor / end > 22500 && fabs(flat) <= 2 &&
                      samples[core_count * 9 / 10] - samples[core_count / 10] <= 2;
        if (marine) {
            river = false;
            flat = 0;
        }
        if (!river) {
            HydroBody body = {.heads_m = {(float)flat}, .flags = marine ? 2 : 0};
            if (!append_body(&result, body, queue, end))
                goto failed;
            continue;
        }
        double minimum = 1e30, maximum = -1e30;
        for (size_t j = 0; j < end; j++) {
            double t = axis_at(queue[j], w, dx, dy, mx, my, ax, ay);
            minimum = fmin(minimum, t);
            maximum = fmax(maximum, t);
        }
        double sums[HEAD_BINS] = {0}, weights[HEAD_BINS] = {0}, heads[HEAD_BINS];
        double span = fmax(1, maximum - minimum);
        for (size_t j = 0; j < end; j++) {
            size_t i = queue[j];
            if (!isfinite(elevation[i]))
                continue;
            int bin = (int)fmin(HEAD_BINS - 1,
                                (axis_at(i, w, dx, dy, mx, my, ax, ay) - minimum) / span * HEAD_BINS);
            double weight = fmax(1, result.shore_m[i]);
            weight *= weight * weight;
            sums[bin] += elevation[i] * weight;
            weights[bin] += weight;
        }
        for (int b = 0; b < HEAD_BINS; b++) {
            if (weights[b])
                heads[b] = sums[b] / weights[b];
            else {
                int nearest_bin = -1;
                for (int j = 0; j < HEAD_BINS; j++)
                    if (weights[j] && (nearest_bin < 0 || abs(j - b) < abs(nearest_bin - b)))
                        nearest_bin = j;
                heads[b] = nearest_bin >= 0 ? sums[nearest_bin] / weights[nearest_bin] : flat;
            }
        }
        /* Remove uphill oscillations while retaining a longitudinal head drop.
           Pool-adjacent-violators fits the source's dominant flow direction. */
        double sign = heads[HEAD_BINS - 1] >= heads[0] ? 1 : -1;
        double values[HEAD_BINS], mass[HEAD_BINS];
        int first[HEAD_BINS], last[HEAD_BINS], blocks = 0;
        for (int b = 0; b < HEAD_BINS; b++) {
            values[blocks] = heads[b] * sign;
            mass[blocks] = fmax(1, weights[b]);
            first[blocks] = last[blocks] = b;
            blocks++;
            while (blocks > 1 && values[blocks - 2] > values[blocks - 1]) {
                int a = blocks - 2, c = blocks - 1;
                values[a] = (values[a] * mass[a] + values[c] * mass[c]) / (mass[a] + mass[c]);
                mass[a] += mass[c];
                last[a] = last[c];
                blocks--;
            }
        }
        for (int b = 0; b < blocks; b++)
            for (int j = first[b]; j <= last[b]; j++)
                heads[j] = values[b] * sign;
        HydroBody body = {.center_x_m = mx,
                          .center_y_m = my,
                          .axis_x = ax,
                          .axis_y = ay,
                          .minimum_m = minimum,
                          .span_m = span,
                          .flags = 1};
        double maximum_step = .02 * span / (HEAD_BINS - 1);
        for (int b = 0; b < HEAD_BINS; b++) {
            if (b)
                heads[b] = sign > 0 ? fmin(heads[b], heads[b - 1] + maximum_step)
                                    : fmax(heads[b], heads[b - 1] - maximum_step);
            body.heads_m[b] = (float)heads[b];
        }
        if (!append_body(&result, body, queue, end))
            goto failed;
    }
    for (size_t i = 0; i < count; i++)
        nearest[i] = water[i] ? i : SIZE_MAX;
    nearest_pass(nearest, w, h, dx, dy, false);
    nearest_pass(nearest, w, h, dx, dy, true);
    for (size_t i = 0; i < count; i++)
        if (!water[i] && nearest[i] != SIZE_MAX) {
            double x = ((double)(i % w) - nearest[i] % w) * dx, y = ((double)(i / w) - nearest[i] / w) * dy;
            result.shore_m[i] = (float)-fmax(0, hypot(x, y) - half);
            result.body_ids[i] = result.body_ids[nearest[i]];
        } else if (!water[i])
            result.shore_m[i] = -1e6f;
    free(nearest);
    free(queue);
    free(visited);
    free(samples);
    *out = result;
    return true;
failed:
    free(nearest);
    free(queue);
    free(visited);
    free(samples);
    hydrology_destroy(&result);
    return false;
}
