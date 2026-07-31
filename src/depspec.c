#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "depspec.h"

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}

int coco_depspec_parse(const char *raw, char *name_out, size_t name_sz,
                        CocoRel *rel_out, char *ver_out, size_t ver_sz) {
    if (rel_out)  *rel_out = COCO_REL_NONE;
    if (ver_out && ver_sz) ver_out[0] = '\0';
    if (name_out && name_sz) name_out[0] = '\0';

    if (!raw || !*raw || !name_out || !name_sz) return -1;

    /* Busca el primer caracter de operador (>, <, =). Todo lo anterior es
     * el nombre; el operador y lo que sigue (sin contar '=' extra tipo "==")
     * es la version. */
    const char *op = NULL;
    for (const char *c = raw; *c; c++) {
        if (*c == '>' || *c == '<' || *c == '=') { op = c; break; }
    }

    if (!op) {
        snprintf(name_out, name_sz, "%s", raw);
        trim(name_out);
        return *name_out ? 0 : -1;
    }

    size_t namelen = (size_t)(op - raw);
    if (namelen >= name_sz) namelen = name_sz - 1;
    memcpy(name_out, raw, namelen);
    name_out[namelen] = '\0';
    trim(name_out);
    if (!*name_out) return -1;

    CocoRel rel;
    const char *ver_start;
    if (op[0] == '>' && op[1] == '=') { rel = COCO_REL_GE; ver_start = op + 2; }
    else if (op[0] == '<' && op[1] == '=') { rel = COCO_REL_LE; ver_start = op + 2; }
    else if (op[0] == '=' && op[1] == '=') { rel = COCO_REL_EQ; ver_start = op + 2; }
    else if (op[0] == '>') { rel = COCO_REL_GT; ver_start = op + 1; }
    else if (op[0] == '<') { rel = COCO_REL_LT; ver_start = op + 1; }
    else { rel = COCO_REL_EQ; ver_start = op + 1; } /* '=' suelto */

    if (rel_out) *rel_out = rel;
    if (ver_out && ver_sz) {
        snprintf(ver_out, ver_sz, "%s", ver_start);
        trim(ver_out);
        if (!*ver_out && rel_out) *rel_out = COCO_REL_NONE; /* "curl>=" mal formado: ignora la version */
    }

    return 0;
}
