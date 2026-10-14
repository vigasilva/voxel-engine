#ifndef MATERIAL_PICKER_H
#define MATERIAL_PICKER_H
#include "material.h"
#include <stddef.h>
typedef struct {
    char query[64];
    uint16_t matches[MAT_COUNT];
    size_t count, cursor;
    bool active, recapture;
} MaterialPicker;
void material_picker_open(MaterialPicker *picker, uint16_t current);
void material_picker_filter(MaterialPicker *picker, const char *query);
void material_picker_append(MaterialPicker *picker, const char *text);
void material_picker_backspace(MaterialPicker *picker);
void material_picker_step(MaterialPicker *picker, int step);
uint16_t material_picker_selected(const MaterialPicker *picker);
#endif
