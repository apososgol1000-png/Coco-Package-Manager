#ifndef COCO_DEPSPEC_H
#define COCO_DEPSPEC_H

#include <stddef.h>

/* Relación de versión declarada en un dependency/conflict spec.
 * COCO_REL_NONE = sin restricción de versión (solo nombre, ej. "curl"). */
typedef enum {
    COCO_REL_NONE = 0,
    COCO_REL_EQ,   /* nombre=version  o  nombre==version */
    COCO_REL_GT,   /* nombre>version  */
    COCO_REL_GE,   /* nombre>=version */
    COCO_REL_LT,   /* nombre<version  */
    COCO_REL_LE,   /* nombre<=version */
} CocoRel;

/* Parsea un spec de dependencia/conflicto tal como viene en manifest.json /
 * index.json, ej: "curl", "curl>=1.2.0", "libfoo <= 2.0", "bar==3".
 *
 * name_out   : nombre del paquete/capacidad (siempre se llena si retorna 0).
 * rel_out    : puede ser NULL si no interesa (ej. cuando solo quieres el
 *              nombre, como en el BFS de autoremove).
 * ver_out    : puede ser NULL si rel_out también es NULL. Si no hay
 *              restricción de versión queda como cadena vacía.
 *
 * Retorna 0 si se pudo extraer un nombre no vacío, -1 si el spec es inválido
 * (raw NULL/vacío, o el nombre quedaría vacío). No valida que la versión en
 * sí tenga formato numérico -- eso lo deja a quien resuelva (libsolv sólo
 * compara componentes tipo "1.2.10" vs "1.9.0" igual que vercmp() en pkg.c). */
int coco_depspec_parse(const char *raw, char *name_out, size_t name_sz,
                        CocoRel *rel_out, char *ver_out, size_t ver_sz);

#endif
