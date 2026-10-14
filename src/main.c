#include "renderer.h"
#include "hud.h"
#include "material_picker_sdl.h"
#include "detail.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>
typedef struct {
    WorldPosition position;
    float yaw, pitch;
} Camera;
static const char *geometry_names[GEOMETRY_VIEW_COUNT] = {"SOLID", "TRIANGLE OVERLAY", "WIREFRAME"};
#define INTERACTION_REACH_METERS 10.0f
static Vec3 camera_direction(const Camera *c) {
    return v3(cosf(c->pitch) * sinf(c->yaw), sinf(c->pitch), -cosf(c->pitch) * cosf(c->yaw));
}
static bool focus_workshop(World *world, Camera *camera) {
    if (!world_create_workshop(world))
        return false;
    CellCoord origin = world_chunk_origin(world, world->detail_chunk);
    if (!position_from_local(origin,
                             dv3(812.5 / VOXEL_SIZE_MM, 835.0 / VOXEL_SIZE_MM, 1065.0 / VOXEL_SIZE_MM),
                             &camera->position))
        return false;
    camera->yaw = 0;
    camera->pitch = -atanf(22.5f / 190.0f);
    return true;
}
static bool parse_number(const char *s, uint32_t *out) {
    errno = 0;
    char *end;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || !s[0] || s[0] == '-' || *end || n > UINT32_MAX)
        return false;
    *out = (uint32_t)n;
    return true;
}
static bool parse_coord(const char *s, int64_t *out) {
    errno = 0;
    char *end;
    intmax_t n = strtoimax(s, &end, 10);
    if (errno || !s[0] || *end || n < INT64_MIN || n > INT64_MAX)
        return false;
    *out = (int64_t)n;
    return true;
}
static void set_capture(bool *capture, bool enabled) {
    if (SDL_SetRelativeMouseMode(enabled ? SDL_TRUE : SDL_FALSE) == 0)
        *capture = enabled;
    else
        fprintf(stderr, "Mouse capture: %s\n", SDL_GetError());
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    bool lab_mode = false;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--lab") || !strcmp(argv[i], "--coarse") || !strcmp(argv[i], "--millimeter") ||
            !strcmp(argv[i], "--full-detail") || !strcmp(argv[i], "--origin-cell") ||
            !strcmp(argv[i], "--smoke-test") || !strcmp(argv[i], "--seed") || !strcmp(argv[i], "--load"))
            lab_mode = true;
    if (!lab_mode)
        return planet_app_main(argc, argv);
    uint32_t seed = 42, frame_limit = 0;
    bool validation = false, smoke = false, vsync = false, workshop = false, coarse = false,
         full_detail = false;
    uint32_t view_m = 512;
    const int brush_sizes[] = {1, 5, 10, 25, 125};
    int brush_index = 0;
    const char *load_path = NULL;
    CellCoord requested_origin = {0};
    bool origin_requested = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--lab"))
            continue;
        else if (!strcmp(argv[i], "--validation"))
            validation = true;
        else if (!strcmp(argv[i], "--full-detail"))
            full_detail = true;
        else if (!strcmp(argv[i], "--view-distance") && i + 1 < argc) {
            if (!parse_number(argv[++i], &view_m) || view_m < 32 || view_m > 1024) {
                fprintf(stderr, "View distance must be 32..1024 meters\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "--coarse"))
            coarse = true;
        else if (!strcmp(argv[i], "--millimeter"))
            workshop = true;
        else if (!strcmp(argv[i], "--vsync"))
            vsync = true;
        else if (!strcmp(argv[i], "--smoke-test")) {
            smoke = true;
            frame_limit = 120;
        } else if ((!strcmp(argv[i], "--seed") || !strcmp(argv[i], "--frames")) && i + 1 < argc) {
            bool is_seed = !strcmp(argv[i], "--seed");
            uint32_t n;
            if (!parse_number(argv[++i], &n)) {
                fprintf(stderr, "Invalid numeric argument\n");
                return 1;
            }
            if (is_seed)
                seed = n;
            else
                frame_limit = n;
        } else if (!strcmp(argv[i], "--origin-cell") && i + 3 < argc) {
            origin_requested = true;
            if (!parse_coord(argv[++i], &requested_origin.x) ||
                !parse_coord(argv[++i], &requested_origin.y) ||
                !parse_coord(argv[++i], &requested_origin.z)) {
                fprintf(stderr, "Invalid signed 64-bit origin coordinate\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "--load") && i + 1 < argc)
            load_path = argv[++i];
        else if (!strcmp(argv[i], "--help")) {
            puts("Voxel Laboratory\nUsage: voxel_engine [--seed N] [--load FILE] [--validation] [--vsync] "
                 "[--frames N] "
                 "[--smoke-test] [--full-detail] [--view-distance M] [--coarse] [--millimeter] "
                 "[--origin-cell X Y Z]\nWASD fly, Space/Ctrl "
                 "up/down, Shift "
                 "fast, mouse "
                 "look\nLMB remove, RMB "
                 "place, MMB pick, 1-8/wheel material, Tab capture/release\nF5 save world.vxw, F9 load "
                 "world.vxw, F3 geometry, F4 workshop, [/] brush size, Esc release mouse / quit");
            return 0;
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
            return 1;
        }
    }
    if (!materials_validate()) {
        fprintf(stderr, "Invalid material definitions\n");
        return 1;
    }
    World world;
    if (!world_init(&world)) {
        fprintf(stderr, "Cannot allocate world\n");
        return 1;
    }
    if (!load_path && !(coarse        ? world_generate(&world, seed)
                        : full_detail ? world_generate_detailed(&world, seed)
                                      : world_generate_lazy(&world, seed))) {
        fprintf(stderr, "Cannot generate world\n");
        world_destroy(&world);
        return 1;
    }
    char status[128] = "READY TO EXPLORE", error[128];
    if (load_path && !world_load(&world, load_path, error, sizeof(error))) {
        fprintf(stderr, "Load: %s\n", error);
        world_destroy(&world);
        return 1;
    }
    if (origin_requested && !world_rebase(&world, requested_origin)) {
        fprintf(stderr, "World origin leaves insufficient signed 64-bit range for the map and neighbors\n");
        world_destroy(&world);
        return 1;
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        world_destroy(&world);
        return 1;
    }
    SDL_Window *window =
        SDL_CreateWindow("Voxel Laboratory | C + Vulkan", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                         1280, 800, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        fprintf(stderr, "Window: %s\n", SDL_GetError());
        SDL_Quit();
        world_destroy(&world);
        return 1;
    }
    SDL_SetWindowMinimumSize(window, 960, 600);
    Renderer *renderer = renderer_create(window, validation, vsync);
    Hud hud = {0};
    if (!renderer || !hud_init(&hud, HUD_VERTEX_CAPACITY)) {
        hud_destroy(&hud);
        renderer_destroy(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        world_destroy(&world);
        return 1;
    }
    Camera camera = {.yaw = 0, .pitch = -.65f};
    position_from_local(world.origin,
                        dv3(8 / VOXEL_SIZE_METERS, 5.5 / VOXEL_SIZE_METERS, 12.25 / VOXEL_SIZE_METERS),
                        &camera.position);
    if (workshop && !focus_workshop(&world, &camera)) {
        fprintf(stderr, "Could not create the millimeter workshop\n");
        hud_destroy(&hud);
        renderer_destroy(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        world_destroy(&world);
        return 1;
    }
    GeometryView geometry = GEOMETRY_SOLID;
    bool running = true, captured = false, ok = true;
    uint16_t selected = MAT_SOIL;
    if (!frame_limit)
        set_capture(&captured, true);
    uint64_t last = SDL_GetPerformanceCounter(), frequency = SDL_GetPerformanceFrequency();
    float fps = 60, fps_timer = 0, status_timer = 0;
    unsigned fps_frames = 0;
    uint32_t frames = 0;
    printf("World: %zu base cells, %d mm/base cell, seed %u\n", VOXEL_COUNT, VOXEL_SIZE_MM, world.seed);
    printf("Voxel storage: %zu requested bytes; 64-bit origin: %" PRId64 " %" PRId64 " %" PRId64 "\n",
           world_storage_bytes(&world), world.origin.x, world.origin.y, world.origin.z);
    printf("Millimeter storage: %zu/%d chunks, %zu region nodes\n", world_detail_count(&world), CHUNK_COUNT,
           world_detail_nodes(&world));
    printf("Terrain: %s; view %u m; queue <=%d meshes/frame, 4 ms scheduling budget\n",
           coarse || full_detail ? "FULL MAP" : "DYNAMIC LOD / FRUSTUM STREAMING", view_m,
           SCENE_BUILD_BUDGET);
    puts("F3 geometry; F4 workshop; [/] brush size; Tab mouse; F5 save; F9 load.");
    MaterialPicker picker = {0};
    SDL_StopTextInput();
    while (running && (!frame_limit || frames < frame_limit)) {
        uint64_t now = SDL_GetPerformanceCounter();
        float elapsed = (float)((double)(now - last) / (double)frequency);
        last = now;
        float dt = fminf(elapsed, .05f);
        status_timer += elapsed;
        if (status_timer > 5)
            status[0] = 0;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (material_picker_event(&picker, &event, &selected, &captured))
                continue;
            if (event.type == SDL_QUIT)
                running = false;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                set_capture(&captured, false);
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                SDL_Keycode key = event.key.keysym.sym;
                if (key == SDLK_ESCAPE) {
                    if (captured)
                        set_capture(&captured, false);
                    else
                        running = false;
                }
                if (key == SDLK_TAB)
                    set_capture(&captured, !captured);
                if (key == SDLK_F3) {
                    geometry = (GeometryView)((geometry + 1) % GEOMETRY_VIEW_COUNT);
                    printf("Geometry: %s\n", geometry_names[geometry]);
                }
                if (key == SDLK_F4) {
                    if (workshop) {
                        workshop = false;
                        position_from_local(world.origin, dv3(64, 44, 98), &camera.position);
                        camera.yaw = 0;
                        camera.pitch = -.65f;
                    } else if (focus_workshop(&world, &camera))
                        workshop = true;
                    else
                        snprintf(status, sizeof(status), "NO EMPTY CHUNK FOR WORKSHOP");
                    printf("Workshop focus: %s\n", workshop ? "ON" : "OFF");
                }
                if (key == SDLK_LEFTBRACKET && brush_index > 0)
                    brush_index--;
                if (key == SDLK_RIGHTBRACKET && brush_index < 4)
                    brush_index++;
                if (key == SDLK_LEFTBRACKET || key == SDLK_RIGHTBRACKET)
                    printf("Brush: %d mm\n", brush_sizes[brush_index]);
                if (key >= SDLK_1 && key <= SDLK_8)
                    selected = (uint16_t)(key - SDLK_1 + 1);
                if (key == SDLK_F5 || key == SDLK_F9) {
                    DVec3 previous_local = {0};
                    position_relative_cells(camera.position, world.origin, &previous_local);
                    bool success = key == SDLK_F5 ? world_save(&world, "world.vxw", error, sizeof(error))
                                                  : world_load(&world, "world.vxw", error, sizeof(error));
                    if (success && key == SDLK_F9) {
                        position_from_local(world.origin, previous_local, &camera.position);
                        if (world.workshop_chunk < 0)
                            workshop = false;
                    }
                    snprintf(status, sizeof(status), "%s",
                             success ? (key == SDLK_F5 ? "SAVED WORLD.VXW" : "LOADED WORLD.VXW") : error);
                    printf("%s\n", status);
                    status_timer = 0;
                }
            }
            if (event.type == SDL_MOUSEMOTION && captured) {
                camera.yaw += (float)event.motion.xrel * .0025f;
                camera.pitch -= (float)event.motion.yrel * .0025f;
                if (camera.pitch > 1.54f)
                    camera.pitch = 1.54f;
                if (camera.pitch < -1.54f)
                    camera.pitch = -1.54f;
                camera.yaw = remainderf(camera.yaw, 6.2831853f);
            }
            if (event.type == SDL_MOUSEWHEEL) {
                int wheel = event.wheel.y;
                if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
                    wheel = -wheel;
                int next = (int)selected - 1 - wheel % (MAT_COUNT - 1);
                selected = (uint16_t)((next + MAT_COUNT - 1) % (MAT_COUNT - 1) + 1);
            }
            if (event.type == SDL_MOUSEBUTTONDOWN && captured) {
                if (!world_prepare_ray(&world, camera.position, camera_direction(&camera),
                                       INTERACTION_REACH_METERS / VOXEL_SIZE_METERS)) {
                    ok = false;
                    running = false;
                    break;
                }
                RayHit hit = world_raycast(&world, camera.position, camera_direction(&camera),
                                           INTERACTION_REACH_METERS / VOXEL_SIZE_METERS);
                if (hit.hit && hit.fine) {
                    if (event.button.button == SDL_BUTTON_MIDDLE)
                        selected = world_hit_voxel(&world, hit).material;
                    if (event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_RIGHT) {
                        bool place = event.button.button == SDL_BUTTON_RIGHT;
                        MmBox box;
                        if (world_brush_box(hit, brush_sizes[brush_index], place, &box)) {
                            DVec3 p;
                            bool in_camera = false;
                            if (place && position_relative_cells(camera.position,
                                                                 world_chunk_origin(&world, hit.chunk), &p))
                                in_camera =
                                    p.x * VOXEL_SIZE_MM >= box.lo[0] && p.x * VOXEL_SIZE_MM < box.hi[0] &&
                                    p.y * VOXEL_SIZE_MM >= box.lo[1] && p.y * VOXEL_SIZE_MM < box.hi[1] &&
                                    p.z * VOXEL_SIZE_MM >= box.lo[2] && p.z * VOXEL_SIZE_MM < box.hi[2];
                            if (!in_camera &&
                                !world_paint_chunk_mm(&world, hit.chunk, box,
                                                      (Voxel){place ? selected : MAT_AIR, 0, 293.15f})) {
                                snprintf(status, sizeof(status), "MILLIMETER EDIT FAILED");
                                status_timer = 0;
                            }
                        }
                    }
                } else if (hit.hit) {
                    if (event.button.button == SDL_BUTTON_LEFT)
                        world_set(&world, hit.x, hit.y, hit.z, (Voxel){MAT_AIR, 0, 293.15f});
                    if (event.button.button == SDL_BUTTON_MIDDLE)
                        selected = world_get(&world, hit.x, hit.y, hit.z).material;
                    if (event.button.button == SDL_BUTTON_RIGHT && (hit.nx || hit.ny || hit.nz)) {
                        int64_t x = hit.x + hit.nx, y = hit.y + hit.ny, z = hit.z + hit.nz;
                        bool camera_cell = camera.position.cell.x == x && camera.position.cell.y == y &&
                                           camera.position.cell.z == z;
                        if (!camera_cell && world_get(&world, x, y, z).material == MAT_AIR)
                            world_set(&world, x, y, z, (Voxel){selected, 0, 293.15f});
                    }
                }
            }
        }
        if (!running)
            break;
        if (captured) {
            const Uint8 *keys = SDL_GetKeyboardState(NULL);
            Vec3 forward = v3(sinf(camera.yaw), 0, -cosf(camera.yaw)),
                 right = v3(cosf(camera.yaw), 0, sinf(camera.yaw)), move = v3(0, 0, 0);
            if (keys[SDL_SCANCODE_W])
                move = vadd(move, forward);
            if (keys[SDL_SCANCODE_S])
                move = vsub(move, forward);
            if (keys[SDL_SCANCODE_D])
                move = vadd(move, right);
            if (keys[SDL_SCANCODE_A])
                move = vsub(move, right);
            if (keys[SDL_SCANCODE_SPACE])
                move.y += 1;
            if (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL])
                move.y -= 1;
            double speed_mm =
                workshop ? (keys[SDL_SCANCODE_LSHIFT] ? 300 : 60) : (keys[SDL_SCANCODE_LSHIFT] ? 9000 : 3000);
            if (workshop && keys[SDL_SCANCODE_LALT])
                speed_mm = 5;
            Vec3 direction = vnormalize(move);
            position_move_mm(&camera.position, dv3(direction.x * speed_mm * dt, direction.y * speed_mm * dt,
                                                   direction.z * speed_mm * dt));
        }
        if (smoke) {
            if (frames == 10 || frames == 30 || frames == 50) {
                geometry = (GeometryView)((geometry + 1) % GEOMETRY_VIEW_COUNT);
                printf("Smoke geometry: %s\n", geometry_names[geometry]);
            }
            if (frames == 20) {
                world_set(&world, world.origin.x + 63, world.origin.y + 40, world.origin.z + 63,
                          (Voxel){MAT_COPPER, 0, 350});
                puts("Smoke: edited voxel at chunk corner");
            }
            if (frames == 40) {
                SDL_SetWindowSize(window, 1024, 640);
                puts("Smoke: resized window");
            }
            if (frames == 60) {
                SDL_MinimizeWindow(window);
                puts("Smoke: minimized window");
            }
            if (frames == 75) {
                SDL_RestoreWindow(window);
                puts("Smoke: restored window");
            }
            if (frames == 90) {
                world_set(&world, world.origin.x + 63, world.origin.y + 40, world.origin.z + 63,
                          (Voxel){MAT_AIR, 0, 293.15f});
                SDL_SetWindowSize(window, 1280, 800);
                puts("Smoke: removed voxel and restored size");
            }
        }
        /* A minimized window needs no new terrain or geometry. */
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(16);
            frames++;
            continue;
        }
        if (!world_prepare_ray(&world, camera.position, camera_direction(&camera),
                               INTERACTION_REACH_METERS / VOXEL_SIZE_METERS)) {
            ok = false;
            break;
        }
        if (!renderer_sync_world(renderer, &world, camera.position, camera_direction(&camera), (float)view_m,
                                 !coarse && !full_detail)) {
            ok = false;
            break;
        }
        int width, height;
        renderer_size(renderer, &width, &height);
        if (width <= 0 || height <= 0 || (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) {
            SDL_Delay(16);
            frames++;
            continue;
        }
        RayHit hit = world_raycast(&world, camera.position, camera_direction(&camera),
                                   INTERACTION_REACH_METERS / VOXEL_SIZE_METERS);
        fps_timer += elapsed;
        fps_frames++;
        if (fps_timer >= .5f) {
            fps = (float)fps_frames / fps_timer;
            fps_frames = 0;
            fps_timer = 0;
        }
        hud_build(&hud, &world, hit, selected, camera.position, width, height, fps,
                  renderer_vertex_count(renderer), captured, geometry_names[geometry], workshop,
                  brush_sizes[brush_index], status, renderer_scene(renderer));
        RenderPush push = {0};
        push.view_projection = mat_mul(mat_perspective_reverse_z(1.22173f, (float)width / (float)height,
                                                                 .0001f / VOXEL_SIZE_METERS,
                                                                 (float)(view_m + 128) / VOXEL_SIZE_METERS),
                                       mat_view(v3(0, 0, 0), camera_direction(&camera)));
        push.viewport[2] = (float)geometry;
        push.viewport[3] = VOXEL_SIZE_MM / 250.0f;
        push.selected[0] = hit.hit ? (float)(hit.x - world.origin.x) : 0;
        push.selected[1] = hit.hit ? (float)(hit.y - world.origin.y) : 0;
        push.selected[2] = hit.hit ? (float)(hit.z - world.origin.z) : 0;
        push.selected[3] = hit.hit && !hit.fine ? 1 : 0;
        if (hit.hit && hit.fine) {
            CellCoord d = world_chunk_origin(&world, hit.chunk);
            push.fine_target[0] = (float)((d.x - world.origin.x) * VOXEL_SIZE_MM + hit.mm[0]);
            push.fine_target[1] = (float)((d.y - world.origin.y) * VOXEL_SIZE_MM + hit.mm[1]);
            push.fine_target[2] = (float)((d.z - world.origin.z) * VOXEL_SIZE_MM + hit.mm[2]);
            push.fine_target[3] = 1;
        }
        hud_material_picker(&hud, &picker, width, height);
        if (hud.overflow || !renderer_draw(renderer, &push, camera.position, hud.vertices, hud.count)) {
            ok = false;
            break;
        }
        frames++;
    }
    if (frame_limit && ok)
        printf("Completed %u frames successfully (%zu terrain vertices).\n", frames,
               renderer_vertex_count(renderer));
    if (frame_limit)
        printf("Resident fine chunks: %zu, storage %zu bytes; visible %zu, queued %zu; mesh allocations %zu "
               "bytes\n",
               world_detail_count(&world), world_storage_bytes(&world), renderer_scene(renderer)->visible,
               renderer_scene(renderer)->pending, renderer_scene(renderer)->mesh_bytes);
    if (frame_limit) {
        DVec3 local;
        if (position_relative_cells(camera.position, world.origin, &local))
            printf("Camera local meters: %.3f %.3f %.3f\n", local.x * VOXEL_SIZE_METERS,
                   local.y * VOXEL_SIZE_METERS, local.z * VOXEL_SIZE_METERS);
    }
    set_capture(&captured, false);
    hud_destroy(&hud);
    renderer_destroy(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    world_destroy(&world);
    return ok ? 0 : 1;
}
