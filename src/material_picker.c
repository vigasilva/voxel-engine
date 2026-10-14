#include "material_picker.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
static bool contains(const char *text, const char *query, size_t length) {
    for (size_t i = 0; text[i]; i++) {
        size_t j = 0;
        while (j < length && text[i + j] &&
               tolower((unsigned char)text[i + j]) == tolower((unsigned char)query[j]))
            j++;
        if (j == length)
            return true;
    }
    return false;
}
static bool match(const Material *m, const char *query) {
    char text[180];
    snprintf(text, sizeof(text), "%s %s %s", m->name, material_family_name(m->family),
             m->pure_element >= 0 ? element_symbol((ElementId)m->pure_element) : "");
    while (*query) {
        while (*query == ' ')
            query++;
        size_t length = strcspn(query, " ");
        const Element *symbol = NULL;
        if (length && length <= 2)
            for (unsigned n = 1; n <= E_COUNT; n++) {
                const Element *e = element_by_number(n);
                if (strlen(e->symbol) == length && contains(e->symbol, query, length)) {
                    symbol = e;
                    break;
                }
            }
        if (symbol ? m->pure_element != (int16_t)(symbol->atomic_number - 1)
                   : length && !contains(text, query, length))
            return false;
        query += length;
    }
    return true;
}
void material_picker_filter(MaterialPicker *p, const char *query) {
    /* Copy first: callers may pass the picker's current buffer. */
    char copy[sizeof(p->query)];
    snprintf(copy, sizeof(copy), "%s", query ? query : "");
    memcpy(p->query, copy, strlen(copy) + 1);
    p->count = p->cursor = 0;
    for (uint16_t id = 1; id < MAT_COUNT; id++)
        if (match(material_get(id), p->query))
            p->matches[p->count++] = id;
}
void material_picker_open(MaterialPicker *p, uint16_t current) {
    *p = (MaterialPicker){.active = true};
    material_picker_filter(p, "");
    for (size_t i = 0; i < p->count; i++)
        if (p->matches[i] == current)
            p->cursor = i;
}
void material_picker_append(MaterialPicker *p, const char *text) {
    size_t n = strlen(p->query);
    char copy[sizeof(p->query)];
    memcpy(copy, p->query, n + 1);
    while (text && *text && n + 1 < sizeof(copy)) {
        unsigned char c = (unsigned char)*text++;
        if (c >= 32 && c <= 126)
            copy[n++] = (char)c;
    }
    copy[n] = 0;
    material_picker_filter(p, copy);
}
void material_picker_backspace(MaterialPicker *p) {
    size_t n = strlen(p->query);
    if (n)
        p->query[n - 1] = 0;
    material_picker_filter(p, p->query);
}
void material_picker_step(MaterialPicker *p, int step) {
    if (!p->count)
        return;
    int64_t cursor = (int64_t)p->cursor + step;
    p->cursor = (size_t)(cursor < 0 ? 0 : cursor >= (int64_t)p->count ? (int64_t)p->count - 1 : cursor);
}
uint16_t material_picker_selected(const MaterialPicker *p) {
    return p->count && p->cursor < p->count ? p->matches[p->cursor] : MAT_AIR;
}
