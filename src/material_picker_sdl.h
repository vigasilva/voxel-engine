#ifndef MATERIAL_PICKER_SDL_H
#define MATERIAL_PICKER_SDL_H
#include "material_picker.h"
#include <SDL.h>
bool material_picker_event(MaterialPicker *picker, const SDL_Event *event, uint16_t *selected,
                           bool *captured);
#endif
