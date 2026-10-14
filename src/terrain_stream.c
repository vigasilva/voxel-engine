#define _POSIX_C_SOURCE 200809L
#include "terrain_stream.h"
#include <stdio.h>
#ifdef __unix__
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#endif
void terrain_stream_stop(TerrainStream *s) {
#ifdef __unix__
    if (s->process > 0) {
        kill(-(pid_t)s->process, SIGTERM);
        waitpid((pid_t)s->process, NULL, 0);
    }
#endif
    s->process = 0;
    s->age = 0;
}
bool terrain_stream_step(TerrainStream *s, Planet *p, PlanetGeo location, double elapsed) {
    s->cooldown = fmax(0, s->cooldown - elapsed);
#ifdef __unix__
    if (s->process > 0) {
        int status = 0;
        pid_t result = waitpid((pid_t)s->process, &status, WNOHANG);
        s->age += elapsed;
        if (result == (pid_t)s->process) {
            s->process = 0;
            s->age = 0;
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                bool loaded = planet_detail_refresh(p) > 0;
                if (s->requested_fine) {
                    s->checked_lat = s->lat_key;
                    s->checked_lon = s->lon_key;
                    s->fine_checked = true;
                }
                printf("Terrain detail: %zu local packs loaded\n", planet_detail_count(p));
                return loaded;
            }
            puts("Terrain download unavailable; continuing with cached/global data (see terrain-fetch.log)");
            s->cooldown = 120;
        } else if (s->age > 180) {
            terrain_stream_stop(s);
            s->cooldown = 120;
        }
        return false;
    }
    bool conus = location.latitude_deg >= 24 && location.latitude_deg <= 50 &&
                 location.longitude_deg >= -125 && location.longitude_deg <= -66;
    bool fine = false;
    if (s->enabled && conus && location.altitude_m <= 20000)
        fine =
            location.altitude_m - planet_visible_elevation(p, location.latitude_deg, location.longitude_deg) <
            1000;
    int scale = fine ? 100 : 10;
    int lat_key = (int)floor(location.latitude_deg * scale),
        lon_key = (int)floor(location.longitude_deg * scale);
    if (lat_key != s->lat_key || lon_key != s->lon_key || s->key_scale != scale) {
        s->lat_key = lat_key;
        s->lon_key = lon_key;
        s->key_scale = scale;
        s->stable = 0;
    }
    s->stable += elapsed;
    if (!s->enabled || s->cooldown > 0 || s->stable < 2 || location.altitude_m > 20000 ||
        fabs(location.latitude_deg) > 85 || fabs(location.longitude_deg) > 179.9)
        return false;
    if (fine) {
        TerrainSourceInfo source;
        if (planet_detail_source(p, location.latitude_deg, location.longitude_deg, &source) &&
            !source.legacy && source.provider == TERRAIN_PROVIDER_USGS_3DEP &&
            source.effective_resolution_m <= 2 * p->a_m / EARTH_A_METERS &&
            location.longitude_deg >= source.coverage[0] && location.longitude_deg < source.coverage[2] &&
            location.latitude_deg >= source.coverage[1] && location.latitude_deg < source.coverage[3])
            return false;
        if (s->fine_checked && s->checked_lat == lat_key && s->checked_lon == lon_key)
            return false;
    } else if (planet_detail_sample(p, (lat_key + .5) / scale, (lon_key + .5) / scale, NULL, NULL))
        return false;
    char lat[64], lon[64], log[1200];
    snprintf(lat, sizeof(lat), "%.8f", (lat_key + .5) / scale);
    snprintf(lon, sizeof(lon), "%.8f", (lon_key + .5) / scale);
    snprintf(log, sizeof(log), "%s/../terrain-fetch.log", planet_detail_directory(p));
    mkdir(planet_detail_directory(p), 0755);
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        int fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        execlp("python3", "python3", VOXEL_DETAIL_FETCH, "--lat", lat, "--lon", lon, "--output",
               planet_detail_directory(p), "--quality", fine ? "auto" : "glo30", (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        setpgid(pid, pid);
        s->process = pid;
        s->requested_fine = fine;
        s->age = 0;
        printf("Terrain download: %.3f %.3f / %s + WorldCover\n", location.latitude_deg,
               location.longitude_deg, fine ? "finest verified USGS terrain" : "GLO-30 fallback");
    } else
        s->cooldown = 120;
#else
    (void)p;
    (void)location;
#endif
    return false;
}
