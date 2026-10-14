#include "planet.h"
#include "renderer.h"
#include "hud.h"
#include "material_picker_sdl.h"
#include "resources.h"
#include "terrain_stream.h"
#include "solar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define PI 3.14159265358979323846
typedef struct {
    PlanetBody body;
    float yaw, pitch;
    DVec3 flight_up, flight_east, flight_north;
    bool fly;
} PlanetCamera;
typedef struct {
    bool enabled;
    int target;
    DVec3 offset_m, velocity_mps;
    float yaw, pitch;
} SolarCamera;
static void solar_focus(SolarCamera *camera, const SolarSystem *s) {
    const SolarBody *body = &s->bodies[camera->target];
    DVec3 outward =
        camera->target ? pd_normalize(pd_sub(s->bodies[0].position_m, body->position_m)) : dv3(0, -1, 0);
    camera->offset_m = pd_scale(outward, body->radius_m * 5);
    DVec3 aim = pd_scale(outward, -1);
    camera->yaw = (float)atan2(aim.y, aim.x);
    camera->pitch = (float)asin(aim.z);
    camera->velocity_mps = dv3(0, 0, 0);
}
static DVec3 solar_forward(const SolarCamera *camera) {
    return dv3(cos(camera->pitch) * cos(camera->yaw), cos(camera->pitch) * sin(camera->yaw),
               sin(camera->pitch));
}
static const char *views[] = {"SOLID", "TRIANGLE OVERLAY", "WIREFRAME"};
static bool number(const char *s, double *out) {
    char *end;
    errno = 0;
    double n = strtod(s, &end);
    if (errno || !s[0] || *end || !isfinite(n))
        return false;
    *out = n;
    return true;
}
static Vec3 vector(DVec3 d) {
    return v3((float)d.x, (float)d.y, (float)d.z);
}
static void axes(Planet *p, const PlanetCamera *c, DVec3 *up, DVec3 *east, DVec3 *north, DVec3 *forward,
                 DVec3 *right) {
    if (c->fly) {
        *up = c->flight_up;
        *east = c->flight_east;
        *north = c->flight_north;
    } else {
        *up = planet_up(p, planet_position_m(c->body.position));
        PlanetGeo g = planet_geographic(p, planet_position_m(c->body.position));
        double lon = g.longitude_deg * PI / 180;
        *east = dv3(-sin(lon), cos(lon), 0);
        *north = dv3(up->y * east->z - up->z * east->y, up->z * east->x - up->x * east->z,
                     up->x * east->y - up->y * east->x);
    }
    DVec3 tangent = pd_add(pd_scale(*north, cos(c->yaw)), pd_scale(*east, sin(c->yaw)));
    *forward = pd_add(pd_scale(tangent, cos(c->pitch)), pd_scale(*up, sin(c->pitch)));
    *right = pd_add(pd_scale(*east, cos(c->yaw)), pd_scale(*north, -sin(c->yaw)));
}
static void capture_mouse(bool *captured, bool enabled) {
    if (SDL_SetRelativeMouseMode(enabled ? SDL_TRUE : SDL_FALSE) == 0)
        *captured = enabled;
}
static void look_direction(Planet *p, PlanetCamera *c, DVec3 direction) {
    DVec3 up, east, north, forward, right;
    axes(p, c, &up, &east, &north, &forward, &right);
    c->yaw = (float)atan2(pd_dot(direction, east), pd_dot(direction, north));
    c->pitch = (float)asin(fmax(-1, fmin(1, pd_dot(direction, up))));
}
static void set_flight(Planet *p, PlanetCamera *c, bool enabled) {
    if (c->fly == enabled)
        return;
    DVec3 up, east, north, forward, right;
    axes(p, c, &up, &east, &north, &forward, &right);
    if (enabled) {
        /* Capture the walking frame once. Movement never rotates this basis. */
        c->flight_up = up;
        c->flight_east = east;
        c->flight_north = north;
    }
    c->fly = enabled;
    look_direction(p, c, forward);
}
static void look_at_earth(Planet *p, PlanetCamera *c) {
    look_direction(p, c, pd_normalize(pd_scale(planet_position_m(c->body.position), -1)));
}
static bool spawn(Planet *p, PlanetCamera *c, double lat, double lon, double altitude, bool space,
                  bool orbit) {
    double terrain = planet_elevation(p, lat, lon);
    double h = space ? p->a_m * 2 : orbit ? 400000 : altitude;
    if (!planet_set_position(planet_from_geographic(p, lat, lon, terrain + h), &c->body.position))
        return false;
    c->body.velocity_mps = dv3(0, 0, 0);
    c->body.grounded = false;
    c->body.accumulator = 0;
    c->yaw = -(float)(PI * .5);
    c->pitch = space || orbit ? -1.45f : -.3f;
    c->fly = false;
    set_flight(p, c, space);
    if (space || orbit || h > 50000)
        look_at_earth(p, c);
    if (orbit) {
        DVec3 up, east, north, forward, right;
        axes(p, c, &up, &east, &north, &forward, &right);
        double r = pd_length(planet_position_m(c->body.position));
        c->body.velocity_mps = pd_scale(east, sqrt(GRAVITATIONAL_CONSTANT * p->mass_kg / r));
    }
    return true;
}
int planet_app_main(int argc, char **argv) {
    double latitude = 40, longitude = -105, altitude = 1.81, mass = EARTH_MASS_KG, radius = EARTH_A_METERS,
           view_override = 0;
    const char *data = VOXEL_EARTH_DATA, *save = "earth.vxp";
    uint32_t frames_limit = 0;
    bool validation = false, vsync = false, space = false, orbit = false, smoke = false, freecam = false;
    bool online_terrain = true, occlusion = true, solar_paused = false;
    bool render_stats = false;
    double render_stats_elapsed = 0;
    SolarSystem solar;
    solar_init(&solar);
    SolarCamera solar_camera = {.target = 4};
    const double time_scales[] = {1, 3600, 86400, 604800, 31557600};
    size_t time_scale_index = 0;
    double time_scale = 1, observer_proper = 0, observer_coordinate = 0;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        double n;
        if (!strcmp(arg, "--planet") || !strcmp(arg, "--earth"))
            continue;
        else if (!strcmp(arg, "--validation"))
            validation = true;
        else if (!strcmp(arg, "--solar-view"))
            solar_camera.enabled = true;
        else if (!strcmp(arg, "--no-occlusion"))
            occlusion = false;
        else if (!strcmp(arg, "--render-stats"))
            render_stats = true;
        else if (!strcmp(arg, "--terrain-offline"))
            online_terrain = false;
        else if (!strcmp(arg, "--vsync"))
            vsync = true;
        else if (!strcmp(arg, "--orbit-view"))
            space = true;
        else if (!strcmp(arg, "--orbit"))
            orbit = true;
        else if (!strcmp(arg, "--freecam"))
            freecam = true;
        else if (!strcmp(arg, "--planet-smoke-test")) {
            smoke = true;
            frames_limit = 180;
        } else if ((!strcmp(arg, "--earth-data") || !strcmp(arg, "--planet-save")) && i + 1 < argc) {
            if (!strcmp(arg, "--earth-data"))
                data = argv[++i];
            else
                save = argv[++i];
        } else if ((!strcmp(arg, "--lat") || !strcmp(arg, "--lon") || !strcmp(arg, "--altitude") ||
                    !strcmp(arg, "--mass") || !strcmp(arg, "--radius") || !strcmp(arg, "--view-distance") ||
                    !strcmp(arg, "--frames") || !strcmp(arg, "--solar-body") ||
                    !strcmp(arg, "--time-scale")) &&
                   i + 1 < argc) {
            if (!number(argv[++i], &n)) {
                fprintf(stderr, "Invalid numeric option: %s\n", arg);
                return 1;
            }
            if (!strcmp(arg, "--solar-body")) {
                if (n < 0 || n >= SOLAR_COUNT || floor(n) != n)
                    return 1;
                solar_camera.target = (int)n;
            } else if (!strcmp(arg, "--time-scale")) {
                if (n < 0 || n > 31557600)
                    return 1;
                time_scale = n;
                for (size_t j = 0; j < 5; j++)
                    if (time_scales[j] == n)
                        time_scale_index = j;
            } else if (!strcmp(arg, "--lat"))
                latitude = n;
            else if (!strcmp(arg, "--lon"))
                longitude = n;
            else if (!strcmp(arg, "--altitude"))
                altitude = n;
            else if (!strcmp(arg, "--mass"))
                mass = n;
            else if (!strcmp(arg, "--radius"))
                radius = n;
            else if (!strcmp(arg, "--view-distance"))
                view_override = n;
            else {
                if (n < 0 || n > UINT32_MAX || floor(n) != n)
                    return 1;
                frames_limit = (uint32_t)n;
            }
        } else if (!strcmp(arg, "--help")) {
            puts("Earth Voxel Engine | C + Vulkan\nDefault: Earth at physical scale, global sparse 1 mm "
                 "edits, planetary gravity.\n"
                 "Options: --lat DEG --lon DEG --altitude M --orbit-view --orbit --freecam\n"
                 "--earth-data FILE --planet-save FILE --mass KG --radius M --view-distance M\n"
                 "--validation --vsync --frames N --planet-smoke-test --terrain-offline\n"
                 "--solar-view --solar-body 0..8 --time-scale 0..31557600 --no-occlusion --render-stats\n"
                 "--lab selects the previous laboratory (its options remain available).\n"
                 "WASD walk, Space jump, Shift run; F6 fixed-orientation flight, F7 aim at Earth, F8 "
                 "space/ground view\n"
                 "LMB remove, RMB place, MMB pick, wheel/1-9/0 material, [/] brush 1 mm..1 m\n"
                 "B sphere/cube brush (default sphere), Alt slow flight\n"
                 "F1 computer resources, F2 aim Sun, F3 geometry, F4 material slice, PgUp/PgDn depth, -/= "
                 "scan zoom, V water optics, "
                 "F5 flush saved edits, F10 online terrain, F11 solar view, arrows planets, F9 time warp, "
                 "F12 pause, O occlusion, Tab mouse, Esc release/quit. Planet "
                 "edits save "
                 "automatically.");
            return 0;
        } else {
            fprintf(stderr, "Unknown/incomplete planet option: %s (use --lab for laboratory options)\n", arg);
            return 1;
        }
    }
    if (latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180 || altitude < -radius ||
        altitude > 1e9 || (view_override && (view_override < 100 || view_override > 2e8))) {
        fprintf(stderr, "Invalid planet location/view distance\n");
        return 1;
    }
    Planet p;
    if (!planet_init(&p, data, save, mass, radius, radius * EARTH_B_METERS / EARTH_A_METERS)) {
        fprintf(stderr, "Planet: %s\n", p.error);
        planet_destroy(&p);
        return 1;
    }
    solar.bodies[SOLAR_EARTH].mass_kg = p.mass_kg;
    solar.bodies[SOLAR_EARTH].gm = GRAVITATIONAL_CONSTANT * p.mass_kg;
    solar.bodies[SOLAR_EARTH].radius_m = p.a_m;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        planet_destroy(&p);
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow(
        "Earth | 1:1 planet + C/Vulkan voxels", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 800,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    Renderer *renderer = window ? renderer_create(window, validation, vsync) : NULL;
    Hud hud = {0};
    bool ok = renderer && hud_init(&hud, HUD_VERTEX_CAPACITY);
    PlanetCamera camera = {0};
    if (ok)
        ok = spawn(&p, &camera, latitude, longitude, altitude, space, orbit);
    if (freecam)
        set_flight(&p, &camera, true);
    if (!ok) {
        fprintf(stderr, "Cannot initialize planet viewer\n");
        hud_destroy(&hud);
        renderer_destroy(renderer);
        if (window)
            SDL_DestroyWindow(window);
        SDL_Quit();
        planet_destroy(&p);
        return 1;
    }
    if (!renderer_start_mesher(renderer, data, save, &p))
        puts("Terrain mesher: synchronous fallback");
    renderer_occlusion(renderer, occlusion);
    solar_focus(&solar_camera, &solar);
    SDL_SetWindowMinimumSize(window, 960, 600);
    const int brushes[] = {1, 5, 10, 25, 125, 250, 1000};
    int brush = 0;
    uint16_t material = MAT_SOIL;
    GeometryView geometry = GEOMETRY_SOLID;
    int water_preset = 1;
    bool running = true, captured = false, jump = false, spherical = true;
    if (!frames_limit)
        capture_mouse(&captured, true);
    bool performance_view = true, material_view = false, scan_dirty = true;
    TerrainStream terrain_stream = {.enabled = online_terrain};
    ResourceStats resources = {0};
    ResourceScan scan = {0};
    const double depths[] = {0, .001, .005, .025, .125, .25, 1, 5, 10, 50, 100, 1000, 1e6, 4e6};
    const double spans[] = {.024, .24, 2.4, 24, 240};
    size_t scan_depth = 0, scan_zoom = 3;
    double scan_elapsed = 0;
    char status[192] = "NOAA ETOPO / PHYSICAL EARTH SCALE";
    float status_time = 0, fps = 60, fps_time = 0;
    unsigned fps_count = 0;
    uint32_t frame = 0;
    uint64_t frequency = SDL_GetPerformanceFrequency(), last = SDL_GetPerformanceCounter();
    printf("Earth: equatorial radius %.3f km, polar %.3f km, mass %.6e kg\n", p.a_m / 1000, p.b_m / 1000,
           p.mass_kg);
    printf("Elevation: NOAA ETOPO 2022 sampled %u x %u; %zu bytes CPU state\n", p.earth.columns, p.earth.rows,
           planet_storage_bytes(&p));
    printf("Global 1 mm edits: %s; automatic SQLite transactions; resident cache <=%d chunks\n", save,
           PLANET_CACHE_LIMIT);
    printf("Terrain: %zu native 15-arcsecond regional maps, procedural local relief, greedy mesh LOD\n",
           p.region_count);
    printf("Hydrology: %zu global river segments, %zu local 30 m/10 m packs; F10 online terrain %s\n",
           planet_river_count(&p), planet_detail_count(&p), terrain_stream.enabled ? "ON" : "OFF");
    puts("Planet allocation limits: UNLIMITED edit regions, tree nodes, fragments, mesh vertices and GPU "
         "buffers");
    MaterialPicker picker = {0};
    SDL_StopTextInput();
    while (running && (!frames_limit || frame < frames_limit)) {
        uint64_t now = SDL_GetPerformanceCounter();
        double elapsed = (double)(now - last) / frequency;
        last = now;
        double dt = fmin(.05, elapsed);
        scan_elapsed += elapsed;
        status_time += (float)elapsed;
        if (status_time > 5)
            status[0] = 0;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (material_picker_event(&picker, &event, &material, &captured))
                continue;
            if (event.type == SDL_QUIT)
                running = false;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                capture_mouse(&captured, false);
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                SDL_Keycode key = event.key.keysym.sym;
                if (key == SDLK_ESCAPE) {
                    if (captured)
                        capture_mouse(&captured, false);
                    else
                        running = false;
                }
                if (key == SDLK_TAB)
                    capture_mouse(&captured, !captured);
                if (key == SDLK_F11) {
                    solar_camera.enabled = !solar_camera.enabled;
                    if (solar_camera.enabled) {
                        terrain_stream_stop(&terrain_stream);
                        solar_focus(&solar_camera, &solar);
                    }
                    printf("Solar view: %s\n", solar_camera.enabled ? "ON" : "OFF");
                }
                if (solar_camera.enabled && (key == SDLK_LEFT || key == SDLK_RIGHT)) {
                    solar_camera.target = (solar_camera.target + (key == SDLK_RIGHT ? 1 : 8)) % 9;
                    solar_focus(&solar_camera, &solar);
                    printf("Solar focus: %s\n", solar.bodies[solar_camera.target].name);
                }
                if (key == SDLK_F9) {
                    time_scale_index = (time_scale_index + 1) % 5;
                    time_scale = time_scales[time_scale_index];
                    printf("Solar time scale: %.0f X\n", time_scale);
                }
                if (key == SDLK_F12)
                    solar_paused = !solar_paused;
                if (key == SDLK_o) {
                    occlusion = !occlusion;
                    renderer_occlusion(renderer, occlusion);
                    printf("Occlusion: %s\n", occlusion ? "ON" : "OFF");
                }
                if (key == SDLK_v) {
                    water_preset = (water_preset + 1) % 3;
                    WaterOptics optics = water_default_optics();
                    optics.turbidity = water_preset == 0 ? 0 : water_preset == 1 ? .3f : 3;
                    if (water_preset == 2)
                        optics.scattering_color = v3(.24f, .17f, .08f);
                    renderer_water_optics(renderer, optics);
                    snprintf(status, sizeof(status), "WATER OPTICS: %s",
                             water_preset == 0   ? "CLEAR"
                             : water_preset == 1 ? "DEFAULT"
                                                 : "MURKY");
                    status_time = 0;
                    puts(status);
                }
                if (key == SDLK_F10) {
                    terrain_stream.enabled = !terrain_stream.enabled;
                    if (!terrain_stream.enabled)
                        terrain_stream_stop(&terrain_stream);
                    printf("Online terrain: %s\n", terrain_stream.enabled ? "ON" : "OFF");
                }
                if (key == SDLK_F1) {
                    performance_view = !performance_view;
                    printf("Computer resources: %s\n", performance_view ? "ON" : "OFF");
                }
                if (key == SDLK_F4) {
                    material_view = !material_view;
                    scan_dirty = true;
                    printf("Material visualizer: %s\n", material_view ? "ON" : "OFF");
                }
                if (key == SDLK_PAGEDOWN && scan_depth + 1 < sizeof(depths) / sizeof(depths[0])) {
                    scan_depth++;
                    scan_dirty = true;
                }
                if (key == SDLK_PAGEUP && scan_depth > 0) {
                    scan_depth--;
                    scan_dirty = true;
                }
                if (key == SDLK_MINUS && scan_zoom > 0) {
                    scan_zoom--;
                    scan_dirty = true;
                }
                if (key == SDLK_EQUALS && scan_zoom + 1 < sizeof(spans) / sizeof(spans[0])) {
                    scan_zoom++;
                    scan_dirty = true;
                }
                if (material_view &&
                    (key == SDLK_PAGEUP || key == SDLK_PAGEDOWN || key == SDLK_MINUS || key == SDLK_EQUALS))
                    printf("Material slice: depth %.3f m, width %.3f m\n", depths[scan_depth],
                           spans[scan_zoom]);
                if (key == SDLK_F3) {
                    geometry = (GeometryView)((geometry + 1) % GEOMETRY_VIEW_COUNT);
                    printf("Geometry: %s\n", views[geometry]);
                }
                if (key == SDLK_F6 && !solar_camera.enabled) {
                    set_flight(&p, &camera, !camera.fly);
                    camera.body.velocity_mps = dv3(0, 0, 0);
                    camera.body.accumulator = 0;
                    printf("Planet flight: %s\n",
                           camera.fly ? "ON / FIXED SPACE ORIENTATION" : "OFF / GROUND ALIGNED");
                }
                if (key == SDLK_F7) {
                    if (solar_camera.enabled)
                        solar_focus(&solar_camera, &solar);
                    else
                        look_at_earth(&p, &camera);
                    if (solar_camera.enabled)
                        printf("Solar camera: REFOCUS %s\n", solar.bodies[solar_camera.target].name);
                    else
                        puts("Planet camera: AIM AT EARTH");
                }
                if (key == SDLK_F2) {
                    if (solar_camera.enabled) {
                        DVec3 observer =
                            pd_add(solar.bodies[solar_camera.target].position_m, solar_camera.offset_m);
                        DVec3 aim = pd_normalize(pd_sub(solar.bodies[0].position_m, observer));
                        solar_camera.yaw = (float)atan2(aim.y, aim.x);
                        solar_camera.pitch = (float)asin(aim.z);
                    } else
                        look_direction(&p, &camera,
                                       solar_sun_direction(&solar, planet_position_m(camera.body.position)));
                    puts("Planet camera: AIM AT SUN");
                }
                if (key == SDLK_b) {
                    spherical = !spherical;
                    printf("Planet brush shape: %s\n", spherical ? "SPHERE" : "CUBE");
                }
                if (key == SDLK_F8 && !solar_camera.enabled) {
                    PlanetGeo geo = planet_geographic(&p, planet_position_m(camera.body.position));
                    bool to_space = geo.altitude_m < p.a_m;
                    spawn(&p, &camera, latitude, longitude, 1.81, to_space, false);
                    printf("Planet view: %s\n", to_space ? "SPACE" : "GROUND");
                }
                if (key == SDLK_SPACE)
                    jump = true;
                if (key >= SDLK_1 && key <= SDLK_9)
                    material = (uint16_t)(key - SDLK_1 + 1);
                if (key == SDLK_0)
                    material = MAT_ICE;
                if (key == SDLK_LEFTBRACKET && brush > 0)
                    brush--;
                if (key == SDLK_RIGHTBRACKET && brush < 6)
                    brush++;
                if (key == SDLK_LEFTBRACKET || key == SDLK_RIGHTBRACKET)
                    printf("Planet brush: %d mm\n", brushes[brush]);
                if (key == SDLK_F5) {
                    bool saved = planet_checkpoint(&p);
                    snprintf(status, sizeof(status), "%s", saved ? "PLANET EDITS FLUSHED" : p.error);
                    status_time = 0;
                    printf("%s\n", status);
                }
            }
            if (event.type == SDL_MOUSEMOTION && captured && solar_camera.enabled) {
                solar_camera.yaw += event.motion.xrel * .0025f;
                solar_camera.pitch =
                    fmaxf(-1.54f, fminf(1.54f, solar_camera.pitch - event.motion.yrel * .0025f));
            }
            if (event.type == SDL_MOUSEMOTION && captured && !solar_camera.enabled) {
                camera.yaw += event.motion.xrel * .0025f;
                camera.pitch -= event.motion.yrel * .0025f;
                camera.pitch = fmaxf(-1.54f, fminf(1.54f, camera.pitch));
                camera.yaw = remainderf(camera.yaw, (float)(2 * PI));
            }
            if (event.type == SDL_MOUSEWHEEL) {
                int next = (int)material - 1 - event.wheel.y;
                material = (uint16_t)((next % (MAT_COUNT - 1) + (MAT_COUNT - 1)) % (MAT_COUNT - 1) + 1);
            }
            if (event.type == SDL_MOUSEBUTTONDOWN && captured && !solar_camera.enabled) {
                DVec3 up, east, north, forward, right;
                axes(&p, &camera, &up, &east, &north, &forward, &right);
                PlanetHit hit = planet_pick(&p, planet_position_m(camera.body.position), forward, 10);
                if (event.button.button == SDL_BUTTON_MIDDLE && hit.hit)
                    material = hit.value.material;
                if (hit.hit &&
                    (event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_RIGHT)) {
                    PlanetBox box;
                    bool place = event.button.button == SDL_BUTTON_RIGHT;
                    if (spherical ? planet_sphere_brush(hit, brushes[brush], place, &box)
                                  : planet_brush(hit, brushes[brush], place, &box)) {
                        DVec3 position = planet_position_m(camera.body.position);
                        double xyz[3] = {position.x * 1000, position.y * 1000, position.z * 1000};
                        bool inside = true;
                        for (int a = 0; a < 3; a++)
                            inside &= xyz[a] >= box.lo[a] && xyz[a] < box.hi[a];
                        if (!place || !inside) {
                            Voxel value = {place ? material : MAT_AIR, 0, 288.15f};
                            bool painted = spherical ? planet_paint_sphere(&p, box, value)
                                                     : planet_paint(&p, box, value);
                            snprintf(status, sizeof(status), "%s",
                                     painted ? "PLANET EDIT AUTO SAVED" : p.error);
                            status_time = 0;
                            printf("%s\n", status);
                        }
                    }
                }
            }
        }
        if (!running)
            break;
        DVec3 up, east, north, forward, right;
        axes(&p, &camera, &up, &east, &north, &forward, &right);
        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        if (solar_camera.enabled) {
            forward = solar_forward(&solar_camera);
            up = dv3(0, 0, 1);
            right = pd_normalize(dv3(forward.y, -forward.x, 0));
        }
        DVec3 move = dv3(0, 0, 0);
        if (captured) {
            DVec3 horizontal = pd_normalize(pd_sub(forward, pd_scale(up, pd_dot(forward, up))));
            if (keys[SDL_SCANCODE_W])
                move = pd_add(move, (camera.fly || solar_camera.enabled) ? forward : horizontal);
            if (keys[SDL_SCANCODE_S])
                move = pd_sub(move, (camera.fly || solar_camera.enabled) ? forward : horizontal);
            if (keys[SDL_SCANCODE_D])
                move = pd_add(move, right);
            if (keys[SDL_SCANCODE_A])
                move = pd_sub(move, right);
            if ((camera.fly || solar_camera.enabled) && keys[SDL_SCANCODE_SPACE])
                move = pd_add(move, up);
            if ((camera.fly || solar_camera.enabled) && keys[SDL_SCANCODE_LCTRL])
                move = pd_sub(move, up);
        }
        if (smoke) {
            if (frame == 20) {
                camera.fly = false;
                spawn(&p, &camera, latitude, longitude, 10, false, false);
                puts("Planet smoke: free fall");
            }
            if (frame == 60) {
                PlanetHit hit =
                    planet_raycast(&p, planet_position_m(camera.body.position),
                                   pd_scale(planet_up(&p, planet_position_m(camera.body.position)), -1), 20);
                PlanetBox b;
                if (planet_brush(hit, 1, false, &b) && planet_paint(&p, b, (Voxel){MAT_AIR, 0, 288.15f}))
                    puts("Planet smoke: 1 mm global carve saved");
            }
            if (frame == 90) {
                spawn(&p, &camera, latitude, longitude, 1.81, true, false);
                puts("Planet smoke: orbital overview");
            }
            if (frame == 150) {
                spawn(&p, &camera, -33.8, 151.2, 1.81, false, false);
                puts("Planet smoke: moved to another continent");
            }
        }
        PlanetGeo geo = planet_geographic(&p, planet_position_m(camera.body.position));
        if (solar_camera.enabled) {
            camera.body.velocity_mps = dv3(0, 0, 0);
        } else if (camera.fly) {
            double speed = fmax(20, fmin(2e6, fmax(0, geo.altitude_m) * .4));
            if (keys[SDL_SCANCODE_LSHIFT])
                speed *= 4;
            if (keys[SDL_SCANCODE_LALT])
                speed = .005;
            position_move_mm(&camera.body.position, pd_scale(pd_normalize(move), speed * dt * 1000));
            camera.body.velocity_mps = pd_scale(pd_normalize(move), speed);
            camera.body.grounded = false;
        } else
            planet_body_step(&p, &camera.body,
                             pd_scale(pd_normalize(move), keys[SDL_SCANCODE_LSHIFT] ? 7 : 3), jump, dt);
        double orbit_dt = solar_paused ? 0 : fmin(.25, elapsed) * time_scale;
        if (!solar_step(&solar, orbit_dt)) {
            ok = false;
            break;
        }
        DVec3 observer =
            pd_add(solar.bodies[SOLAR_EARTH].position_m, planet_position_m(camera.body.position));
        DVec3 observer_velocity =
            relativity_add_velocity(solar.bodies[SOLAR_EARTH].velocity_mps, camera.body.velocity_mps);
        if (solar_camera.enabled) {
            double speed = fmin(.25 * LIGHT_SPEED_MPS, fmax(100, pd_length(solar_camera.offset_m) * .4));
            if (keys[SDL_SCANCODE_LSHIFT])
                speed = fmin(.9 * LIGHT_SPEED_MPS, speed * 8);
            if (keys[SDL_SCANCODE_LALT])
                speed = 10;
            solar_camera.velocity_mps = pd_scale(pd_normalize(move), speed);
            DVec3 next = pd_add(solar_camera.offset_m, pd_scale(solar_camera.velocity_mps, dt));
            /* The inspection camera cannot enter an analytic planet's interior. */
            double minimum = solar.bodies[solar_camera.target].radius_m * 1.001;
            if (pd_length(next) < minimum)
                next = pd_scale(pd_normalize(next), minimum);
            solar_camera.offset_m = next;
            observer = pd_add(solar.bodies[solar_camera.target].position_m, next);
            observer_velocity = relativity_add_velocity(solar.bodies[solar_camera.target].velocity_mps,
                                                        solar_camera.velocity_mps);
        }
        double observer_speed = pd_length(observer_velocity);
        double clock_rate = relativity_clock_rate(solar_potential(&solar, observer, -1), observer_speed);
        observer_coordinate += orbit_dt;
        observer_proper += orbit_dt * clock_rate;
        jump = false;
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(16);
            frame++;
            continue;
        }
        axes(&p, &camera, &up, &east, &north, &forward, &right);
        DVec3 position = planet_position_m(camera.body.position);
        geo = planet_geographic(&p, position);
        double old_ground = planet_elevation(&p, geo.latitude_deg, geo.longitude_deg);
        PlanetGeo detail_location = geo;
        if (planet_detail_sample(&p, (floor(geo.latitude_deg * 10) + .5) / 10,
                                 (floor(geo.longitude_deg * 10) + .5) / 10, NULL, NULL))
            detail_location = planet_geographic(&p, pd_add(position, pd_scale(forward, 1000)));
        if (!solar_camera.enabled && terrain_stream_step(&terrain_stream, &p, detail_location, elapsed)) {
            if (!camera.fly && geo.altitude_m - old_ground < 10) {
                double ground = planet_elevation(&p, geo.latitude_deg, geo.longitude_deg);
                if (geo.altitude_m < ground + 1.8) {
                    planet_set_position(
                        planet_from_geographic(&p, geo.latitude_deg, geo.longitude_deg, ground + 1.802),
                        &camera.body.position);
                    camera.body.velocity_mps = dv3(0, 0, 0);
                    position = planet_position_m(camera.body.position);
                }
            }
            scan_dirty = true;
        }
        double view = planet_view_distance(&p, camera.body.position, view_override);
        PlanetHit hit = solar_camera.enabled ? (PlanetHit){0} : planet_pick(&p, position, forward, 10);
        if ((!solar_camera.enabled || frame == 0) &&
            !renderer_sync_planet(renderer, &p, camera.body.position, vector(forward), vector(up), view,
                                  hit)) {
            fprintf(stderr, "Planet rendering: %s\n", p.error);
            ok = false;
            break;
        }
        int width, height;
        renderer_size(renderer, &width, &height);
        if (width <= 0 || height <= 0) {
            SDL_Delay(16);
            frame++;
            continue;
        }
        fps_time += (float)elapsed;
        fps_count++;
        if (fps_time >= .5f) {
            fps = fps_count / fps_time;
            fps_time = 0;
            fps_count = 0;
        }
        if (!solar_camera.enabled && material_view && (scan_dirty || scan_elapsed >= .5)) {
            DVec3 center;
            if (hit.hit)
                center = dv3((hit.mm[0] + .5) * .001, (hit.mm[1] + .5) * .001, (hit.mm[2] + .5) * .001);
            else {
                center =
                    planet_from_geographic(&p, geo.latitude_deg, geo.longitude_deg,
                                           planet_visible_elevation(&p, geo.latitude_deg, geo.longitude_deg));
                center = pd_sub(center, pd_scale(planet_up(&p, center), .0005));
            }
            DVec3 tangent = pd_normalize(pd_sub(forward, pd_scale(up, pd_dot(forward, up))));
            if (pd_length(tangent) < .5)
                tangent = north;
            p.error[0] = 0;
            if (!resources_scan(&scan, &p, center, right, tangent, depths[scan_depth], spans[scan_zoom])) {
                scan.ready = false;
                snprintf(status, sizeof(status), "%s", p.error[0] ? p.error : "MATERIAL SCAN UNAVAILABLE");
                status_time = 0;
            }
            scan_dirty = false;
            scan_elapsed = 0;
        }
        resources_sample(&resources, elapsed, 1000.0 * (SDL_GetPerformanceCounter() - now) / frequency);
        renderer_resources(renderer, &resources);
        char terrain_status[40];
        if (terrain_stream.process)
            snprintf(terrain_status, sizeof(terrain_status), "LOADING");
        else if (!terrain_stream.enabled)
            snprintf(terrain_status, sizeof(terrain_status), "OFF");
        else if (terrain_stream.cooldown > 0)
            snprintf(terrain_status, sizeof(terrain_status), "RETRY %.0FS", terrain_stream.cooldown);
        else
            snprintf(terrain_status, sizeof(terrain_status), "ON");
        hud_build_planet(&hud, &p, &camera.body, hit, material, width, height, fps,
                         renderer_vertex_count(renderer), renderer_planet_scene(renderer), camera.fly,
                         brushes[brush], spherical, views[geometry], status, terrain_status);
        if (solar_camera.enabled)
            hud_build_solar(&hud, &solar, solar_camera.target, width, height, fps,
                            solar_paused ? 0 : time_scale, observer_speed, clock_rate,
                            observer_coordinate - observer_proper);
        else
            hud_relativity(&hud, width, height, solar.time_s, solar_paused ? 0 : time_scale, clock_rate,
                           observer_coordinate - observer_proper);
        hud_resources(&hud, &resources, &scan, width, height, performance_view,
                      !solar_camera.enabled && material_view, material);
        renderer_solar_view(renderer, &solar, observer, position, solar_camera.enabled);
        if (solar_camera.enabled) {
            forward = solar_forward(&solar_camera);
            up = dv3(0, 0, 1);
        }
        RenderPush push = {0};
        push.view_projection =
            mat_mul(mat_perspective_reverse_z(1.22173f, (float)width / height,
                                              (float)planet_near_distance(&p, camera.body.position) /
                                                  VOXEL_SIZE_METERS,
                                              (float)(view + p.a_m * .01) / VOXEL_SIZE_METERS),
                    mat_view_up(v3(0, 0, 0), vector(forward), vector(up)));
        push.viewport[2] = (float)geometry;
        push.viewport[3] = material_view ? -1 : 0;
        hud_material_picker(&hud, &picker, width, height);
        if (hud.overflow || !renderer_draw(renderer, &push, camera.body.position, hud.vertices, hud.count)) {
            ok = false;
            break;
        }
        frame++;
        render_stats_elapsed += elapsed;
        if (render_stats && render_stats_elapsed >= .5) {
            const PlanetScene *scene = renderer_planet_scene(renderer);
            printf("Render stats: frame=%u mode=%s visible=%zu pending=%zu triangles=%zu cache=%zu "
                   "cache_mib=%.2f plans=%llu fallback=%zu fields=%zu field_uploads=%llu\n",
                   frame,
                   solar_camera.enabled     ? "solar"
                   : resources.texture_only ? "texture"
                                            : "terrain",
                   scene->visible, scene->pending, renderer_vertex_count(renderer) / 3,
                   resources.cached_meshes, resources.cached_mesh_bytes / 1048576.0,
                   (unsigned long long)scene->planning_count, resources.fallback_meshes,
                   resources.terrain_color_fields, (unsigned long long)resources.terrain_color_uploads);
            fflush(stdout);
            render_stats_elapsed = 0;
        }
    }
    printf("Solar: J2000 + %.6f days; proper clock deficit %.9f s; time scale %.0f X\n", solar.time_s / 86400,
           observer_coordinate - observer_proper, time_scale);
    terrain_stream_stop(&terrain_stream);
    planet_checkpoint(&p);
    PlanetGeo final = planet_geographic(&p, planet_position_m(camera.body.position));
    printf("Planet completed %u frames; %.6f %.6f altitude %.3f m, gravity %.6f m/s2\n", frame,
           final.latitude_deg, final.longitude_deg, final.altitude_m,
           pd_length(planet_gravity(&p, planet_position_m(camera.body.position))));
    printf("Planet memory: %zu CPU bytes, %zu mesh bytes; patches %zu queued %zu; saved %zu, resident %zu "
           "chunks\n",
           planet_storage_bytes(&p), renderer_scene(renderer)->mesh_bytes,
           renderer_planet_scene(renderer)->visible, renderer_planet_scene(renderer)->pending,
           planet_saved_chunks(&p), planet_resident_chunks(&p));
    renderer_resources(renderer, &resources);
    printf("Rendering: %s\n",
           resources.texture_only ? "DISTANT ENGINE TEXTURE / ZERO TERRAIN MESHES" : "TERRAIN MESHES");
    printf("Occlusion: %zu hidden, maximum %zu patches; drawn %zu vertices\n", resources.occluded_patches,
           resources.occluded_max, renderer_vertex_count(renderer));
    capture_mouse(&captured, false);
    hud_destroy(&hud);
    renderer_destroy(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    planet_destroy(&p);
    return ok ? 0 : 1;
}
