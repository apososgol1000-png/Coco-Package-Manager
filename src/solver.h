#ifndef COCO_SOLVER_H
#define COCO_SOLVER_H

#include <jansson.h>
#include <sqlite3.h>

typedef struct {
    char name[128];
    char version[64];   /* version disponible según el índice */
} CocoPlanStep;

/* Calcula el plan de instalación completo para pkg_name usando libsolv:
 * resuelve dependencias transitivas y devuelve el orden correcto de
 * instalación (las dependencias antes que quien las necesita).
 *
 * idx     = el index.json ya parseado (json_t* de un json_object, name -> {version, dependencies, conflicts, provides, _repo, ...})
 *           OJO: index.json solo trae version/description; las dependencias
 *           se leen de cada manifest.json individual vía manifests_cache
 *           (ver nota abajo). Para v0.4 simplificamos: las dependencias
 *           declaradas deben repetirse también dentro de index.json bajo
 *           "dependencies" para que el solver las vea sin tener que bajar
 *           cada manifest.json por adelantado. Lo mismo aplica desde v0.5
 *           para "conflicts" y "provides": si un manifest.json declara
 *           cualquiera de los tres, el generador del índice (fuera de este
 *           repo) debe copiarlos también dentro de index.json o el solver
 *           simplemente no los va a ver.
 *
 *           Formato de cada entrada en "dependencies"/"conflicts" (desde v0.5):
 *           string plano "nombre" (sin restricción) o "nombre<op>version"
 *           con <op> en {>=, <=, ==, =, >, <}, ej: "libfoo>=1.2.0". Ver
 *           depspec.h para el parser exacto. "provides" son strings planos
 *           (capacidades virtuales, sin operador de versión).
 * db      = conexión SQLite abierta (para saber qué ya está instalado, y con
 *           qué conflictos/provides -- ver tablas `conflicts`/`provides` en pkg.c)
 * targets  = arreglo de paquetes que se quieren instalar (desde v0.6 puede
 *           ser más de uno: 'coco install a b c' arma UNA sola transacción
 *           que los resuelve a los tres juntos -- así libsolv detecta
 *           conflictos ENTRE ellos y comparte dependencias repetidas en vez
 *           de resolver cada uno por separado).
 * ntargets = cuántos hay en `targets` (mínimo 1).
 *
 * out_steps: array reservado con malloc (el caller debe hacer free()),
 *            en el orden en que deben instalarse.
 * out_count: cuántos pasos hay.
 *
 * Retorna 0 en éxito, -1 si hubo un problema de dependencias O de conflictos,
 * o si alguno de los `targets` no existe en el índice
 * (el mensaje de libsolv ya se imprimió a stderr). */
int solver_plan_install(json_t *idx, sqlite3 *db, const char **targets, int ntargets,
                         CocoPlanStep **out_steps, int *out_count);

#endif
