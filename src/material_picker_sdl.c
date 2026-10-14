#include "material_picker_sdl.h"
#include <stdio.h>
bool material_picker_event(MaterialPicker *p, const SDL_Event *e, uint16_t *selected, bool *captured) {
    if (!p->active && e->type == SDL_KEYDOWN && e->key.keysym.sym == SDLK_m && !e->key.repeat) {
        bool restore = *captured;
        material_picker_open(p, *selected);
        p->recapture = restore;
        SDL_SetRelativeMouseMode(SDL_FALSE);
        *captured = false;
        SDL_StartTextInput();
        return true;
    }
    if (!p->active)
        return false;
    if (e->type == SDL_TEXTINPUT)
        material_picker_append(p, e->text.text);
    if (e->type == SDL_MOUSEWHEEL)
        material_picker_step(p, -e->wheel.y);
    if (e->type == SDL_KEYDOWN) {
        SDL_Keycode key = e->key.keysym.sym;
        if (key == SDLK_UP || key == SDLK_DOWN)
            material_picker_step(p, key == SDLK_UP ? -1 : 1);
        if (key == SDLK_PAGEUP || key == SDLK_PAGEDOWN)
            material_picker_step(p, key == SDLK_PAGEUP ? -10 : 10);
        if (key == SDLK_BACKSPACE)
            material_picker_backspace(p);
        if (key == SDLK_ESCAPE || (key == SDLK_RETURN && p->count)) {
            if (key == SDLK_RETURN) {
                *selected = material_picker_selected(p);
                printf("Material selected: %u / %s\n", *selected, material_get(*selected)->name);
            }
            p->active = false;
            SDL_StopTextInput();
            if (SDL_SetRelativeMouseMode(p->recapture ? SDL_TRUE : SDL_FALSE) == 0)
                *captured = p->recapture;
        }
    }
    return e->type == SDL_KEYDOWN || e->type == SDL_KEYUP || e->type == SDL_TEXTINPUT ||
           e->type == SDL_MOUSEWHEEL || e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_MOUSEBUTTONUP ||
           e->type == SDL_MOUSEMOTION;
}
