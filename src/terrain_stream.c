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
    int lat_key = (int)floor(location.latitude_deg * 10), lon_key = (int)floor(location.longitude_deg * 10);
    if (lat_key != s->lat_key || lon_key != s->lon_key) {
        s->lat_key = lat_key;
        s->lon_key = lon_key;
        s->stable = 0;
    }
    s->stable += elapsed;
    if (!s->enabled || s->cooldown > 0 || s->stable < 2 || location.altitude_m > 20000 ||
        fabs(location.latitude_deg) > 85 || fabs(location.longitude_deg) > 179.9)
        return false;
    /* Coverage at the cell center identifies complete cached cells rather than
       accepting the halo of a neighboring pack as permanent coverage. */
    if (planet_detail_sample(p, (lat_key + .5) / 10, (lon_key + .5) / 10, NULL, NULL))
        return false;
    char lat[64], lon[64], log[1200];
    snprintf(lat, sizeof(lat), "%.8f", (lat_key + .5) / 10);
    snprintf(lon, sizeof(lon), "%.8f", (lon_key + .5) / 10);
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
               planet_detail_directory(p), (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        setpgid(pid, pid);
        s->process = pid;
        s->age = 0;
        printf("Terrain download: %.3f %.3f / 30 m elevation + 10 m water\n", location.latitude_deg,
               location.longitude_deg);
    } else
        s->cooldown = 120;
#else
    (void)p;
    (void)location;
#endif
    return false;
}
