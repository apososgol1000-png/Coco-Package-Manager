#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <solv/pool.h>
#include <solv/repo.h>
#include <solv/solver.h>
#include <solv/transaction.h>
#include <solv/queue.h>
#include <solv/knownid.h>
#include "solver.h"
#include "depspec.h"

/* Traduce nuestro CocoRel (independiente de libsolv, lo comparte pkg.c) a
 * las banderas REL_* de libsolv. REL_GT/REL_EQ/REL_LT son bits que se
 * combinan: >= es "mayor O igual" (REL_GT|REL_EQ), etc. */
static int rel_to_solv_flags(CocoRel rel) {
    switch (rel) {
        case COCO_REL_EQ: return REL_EQ;
        case COCO_REL_GT: return REL_GT;
        case COCO_REL_GE: return REL_GT | REL_EQ;
        case COCO_REL_LT: return REL_LT;
        case COCO_REL_LE: return REL_LT | REL_EQ;
        default:          return 0;
    }
}

/* Agrega un solvable con name/version a un repo de libsolv.
 * Registramos TANTO el provide plano (name) COMO el versionado (name = evr):
 * el plano es necesario para que las dependencias sin versión ("requiere: curl")
 * encuentren al proveedor; el versionado es para las que sí declaran
 * restricción de versión (desde v0.5, ver apply_requires_str). */
static Id add_solvable(Pool *pool, Repo *repo, const char *name, const char *version) {
    Id p = repo_add_solvable(repo);
    Solvable *s = pool_id2solvable(pool, p);
    s->name = pool_str2id(pool, name, 1);
    s->evr  = pool_str2id(pool, (version && *version) ? version : "0", 1);
    s->arch = ARCH_NOARCH;
    s->provides = repo_addid_dep(repo, s->provides, s->name, 0);
    s->provides = repo_addid_dep(repo, s->provides,
        pool_rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
    /* create=1 aca es importante: sin esto libsolv nunca registra la
     * relacion "nombre = version" como provide propio del solvable, y
     * CUALQUIER requires con restriccion de version ("libfoo>=1.2.0")
     * falla con "nothing provides ..." aunque el paquete si este --
     * porque no tiene de donde comparar versiones. Bug latente desde
     * v0.4 (nunca se ejercito hasta v0.5, que fue cuando agregamos
     * restricciones de version). */
    return p;
}

/* Aplica UN spec de dependencia ("curl" o "curl>=1.2.0") como "requires"
 * de un solvable. Si no hay restricción de versión, el requires queda
 * plano (satisface cualquier versión que provea ese nombre). Compartida
 * entre el repo disponible (viene de JSON) y el repo instalado (viene de
 * la tabla `depends` en SQLite) para que ambos parseen igual. */
static void apply_requires_str(Pool *pool, Repo *repo, Id p, const char *raw) {
    char name[128], ver[64];
    CocoRel rel;
    if (coco_depspec_parse(raw, name, sizeof name, &rel, ver, sizeof ver) != 0) return;

    Solvable *s = pool_id2solvable(pool, p);
    Id nameid = pool_str2id(pool, name, 1);
    Id reqid = nameid;
    if (rel != COCO_REL_NONE && *ver) {
        Id everid = pool_str2id(pool, ver, 1);
        reqid = pool_rel2id(pool, nameid, everid, rel_to_solv_flags(rel), 1);
    }
    s->requires = repo_addid_dep(repo, s->requires, reqid, 0);
}

/* Igual que apply_requires_str pero para conflictos declarados. Un conflicto
 * se dispara si CUALQUIER solvable del pool (disponible O instalado) provee
 * el nombre (con o sin la restricción de version dada). Por eso basta con
 * declararlo de un solo lado (el paquete nuevo "conflicts" contra uno viejo
 * ya instalado) para que libsolv lo detecte en ambas direcciones -- pero
 * igual reflejamos los conflicts de paquetes YA instalados (ver más abajo)
 * para cubrir el caso "el que ya tengo instalado prohíbe este nuevo". */
static void apply_conflicts_str(Pool *pool, Repo *repo, Id p, const char *raw) {
    char name[128], ver[64];
    CocoRel rel;
    if (coco_depspec_parse(raw, name, sizeof name, &rel, ver, sizeof ver) != 0) return;

    Solvable *s = pool_id2solvable(pool, p);
    Id nameid = pool_str2id(pool, name, 1);
    Id confid = nameid;
    if (rel != COCO_REL_NONE && *ver) {
        Id everid = pool_str2id(pool, ver, 1);
        confid = pool_rel2id(pool, nameid, everid, rel_to_solv_flags(rel), 1);
    }
    s->conflicts = repo_addid_dep(repo, s->conflicts, confid, 0);
}

/* Paquete virtual / capacidad: agrega `raw` (nombre plano, sin operador de
 * version -- ver nota en solver.h) como provide extra del solvable, además
 * del name/evr que ya pone add_solvable(). Así "requires: editor" se
 * satisface con cualquier paquete que declare `"provides": ["editor"]`. */
static void apply_provides_str(Pool *pool, Repo *repo, Id p, const char *raw) {
    if (!raw || !*raw) return;
    Solvable *s = pool_id2solvable(pool, p);
    Id nameid = pool_str2id(pool, raw, 1);
    s->provides = repo_addid_dep(repo, s->provides, nameid, 0);
}

static void add_requires(Pool *pool, Repo *repo, Id p, json_t *deps_arr) {
    if (!json_is_array(deps_arr)) return;
    size_t i; json_t *d;
    json_array_foreach(deps_arr, i, d) {
        const char *dn = json_string_value(d);
        if (dn && *dn) apply_requires_str(pool, repo, p, dn);
    }
}

static void add_conflicts(Pool *pool, Repo *repo, Id p, json_t *conf_arr) {
    if (!json_is_array(conf_arr)) return;
    size_t i; json_t *d;
    json_array_foreach(conf_arr, i, d) {
        const char *dn = json_string_value(d);
        if (dn && *dn) apply_conflicts_str(pool, repo, p, dn);
    }
}

static void add_provides(Pool *pool, Repo *repo, Id p, json_t *prov_arr) {
    if (!json_is_array(prov_arr)) return;
    size_t i; json_t *d;
    json_array_foreach(prov_arr, i, d) {
        const char *dn = json_string_value(d);
        if (dn && *dn) apply_provides_str(pool, repo, p, dn);
    }
}

/* Vuelca un SELECT `col` FROM `table` WHERE package = ? sobre cada fila,
 * aplicando `fn` (apply_requires_str / apply_conflicts_str / apply_provides_str)
 * al solvable `p`. Usado para poblar el repo @System desde SQLite, que
 * guarda estos specs como texto plano igual que vienen en manifest.json. */
typedef void (*ApplyFn)(Pool *, Repo *, Id, const char *);
static void apply_from_table(sqlite3 *db, Pool *pool, Repo *repo, Id p,
                              const char *table, const char *name, ApplyFn fn) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT dep FROM %s WHERE package = ?;", table);
    /* nota: las tres tablas (depends/conflicts/provides) usan la columna
     * `dep` para el spec, aunque en `conflicts`/`provides` semánticamente
     * sea "conflict"/"provide" -- mismo nombre de columna a propósito para
     * poder reusar esta función con un solo snprintf. */
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *raw = (const char *)sqlite3_column_text(st, 0);
        if (raw && *raw) fn(pool, repo, p, raw);
    }
    sqlite3_finalize(st);
}

int solver_plan_install(json_t *idx, sqlite3 *db, const char **targets, int ntargets,
                         CocoPlanStep **out_steps, int *out_count) {
    *out_steps = NULL;
    *out_count = 0;

    if (!idx || ntargets <= 0) {
        fprintf(stderr, "[!] Nada que instalar.\n");
        return -1;
    }
    for (int i = 0; i < ntargets; i++) {
        if (!json_object_get(idx, targets[i])) {
            fprintf(stderr, "[!] '%s' no existe en el índice. Ejecuta 'coco sync'.\n", targets[i]);
            return -1;
        }
    }

    Pool *pool = pool_create();
    /* A propósito NO llamamos pool_setarch(): así el filtrado de arquitectura
     * de libsolv queda completamente desactivado (pool->id2arch == NULL) y
     * el comportamiento es idéntico sin importar cómo cada distro compiló
     * libsolv (RPM/DEB tienen noarchid distinto internamente). */

    Repo *avail = repo_create(pool, "coco-available");
    Repo *sys   = repo_create(pool, "@System");
    pool_set_installed(pool, sys);

    /* Repo disponible: todo lo que hay en el índice local */
    const char *key; json_t *val;
    json_object_foreach(idx, key, val) {
        const char *ver = json_string_value(json_object_get(val, "version"));
        Id p = add_solvable(pool, avail, key, ver);
        add_requires(pool, avail, p, json_object_get(val, "dependencies"));
        add_conflicts(pool, avail, p, json_object_get(val, "conflicts"));
        add_provides(pool, avail, p, json_object_get(val, "provides"));
    }

    /* Repo instalado: lo que ya está en SQLite, con sus deps/conflicts/provides */
    if (db) {
        sqlite3_stmt *st;
        sqlite3_prepare_v2(db, "SELECT name, version FROM installed;", -1, &st, NULL);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(st, 0);
            const char *ver  = (const char *)sqlite3_column_text(st, 1);
            Id p = add_solvable(pool, sys, name, ver);

            apply_from_table(db, pool, sys, p, "depends",   name, apply_requires_str);
            apply_from_table(db, pool, sys, p, "conflicts", name, apply_conflicts_str);
            apply_from_table(db, pool, sys, p, "provides",  name, apply_provides_str);
        }
        sqlite3_finalize(st);
    }

    pool_createwhatprovides(pool);

    /* Un job SOLVER_INSTALL por cada target, todos en la MISMA queue: así
     * libsolv arma una sola transacción consistente que los cubre a todos
     * juntos -- comparte dependencias repetidas entre ellos y detecta de
     * una vez si dos de los paquetes pedidos son incompatibles entre sí. */
    Queue job;
    queue_init(&job);
    for (int i = 0; i < ntargets; i++) {
        Id targetid = pool_str2id(pool, targets[i], 0);
        queue_push2(&job, SOLVER_SOLVABLE_NAME | SOLVER_INSTALL, targetid);
    }

    Solver *solv = solver_create(pool);
    int problems = solver_solve(solv, &job);

    if (problems) {
        fprintf(stderr, "[!] No se pudo resolver dependencias:\n");
        for (int i = 1; i <= (int)solver_problem_count(solv); i++)
            fprintf(stderr, "    - %s\n", solver_problem2str(solv, i));
        solver_free(solv);
        queue_free(&job);
        pool_free(pool);
        return -1;
    }

    Transaction *trans = solver_create_transaction(solv);
    transaction_order(trans, 0);

    CocoPlanStep *steps = NULL;
    int count = 0, cap = 0;

    for (int i = 0; i < trans->steps.count; i++) {
        Id p = trans->steps.elements[i];
        Id type = transaction_type(trans, p, SOLVER_TRANSACTION_SHOW_ACTIVE);
        if (type == SOLVER_TRANSACTION_IGNORE) continue;
        if (type & SOLVER_TRANSACTION_ERASE) continue; /* no pedimos borrar nada */

        Solvable *s = pool_id2solvable(pool, p);
        if (count == cap) { cap = cap ? cap * 2 : 8; steps = realloc(steps, cap * sizeof(CocoPlanStep)); }
        snprintf(steps[count].name,    sizeof(steps[count].name),    "%s", pool_id2str(pool, s->name));
        snprintf(steps[count].version, sizeof(steps[count].version), "%s", pool_id2str(pool, s->evr));
        count++;
    }

    transaction_free(trans);
    solver_free(solv);
    queue_free(&job);
    pool_free(pool);

    *out_steps = steps;
    *out_count = count;
    return 0;
}
