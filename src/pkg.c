#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <archive.h>
#include <archive_entry.h>
#include <sqlite3.h>
#include <jansson.h>
#include "net.h"
#include "pkg.h"

/* ─── Rutas ─────────────────────────────────────────────────────────── */
#define COCO_CACHE   "/var/coco/cache"
#define COCO_DB      "/var/coco/db/coco.db"
#define COCO_INDEX   "/var/coco/db/index.json"
#define COCO_LOG_DIR "/var/log/coco"
#define COCO_CONFIG  "/etc/coco.json"

#define MAX_REPOS  8
#define MAX_URL    512

/* ─── Config global ─────────────────────────────────────────────────── */
static char g_repos[MAX_REPOS][MAX_URL];
static int  g_repo_count = 0;

/* ═══════════════════════════════════════════════════════════════════════
 * HELPERS INTERNOS
 * ═══════════════════════════════════════════════════════════════════════ */

/* Crea directorios recursivamente (como mkdir -p) */
static void mkdir_p(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void init_dirs(void) {
    mkdir_p(COCO_CACHE);
    mkdir_p("/var/coco/db");
    mkdir_p(COCO_LOG_DIR);
}

/* Lee un archivo completo a un string en heap. Caller libera la memoria. */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t nread = fread(buf, 1, len, f);
    buf[nread] = '\0';
    fclose(f);
    return buf;
}

/* Compara dos versiones tipo "1.2.10" vs "1.9.0" numéricamente
 * (a diferencia de strcmp, entiende que 10 > 9).
 * Retorna <0 si a<b, 0 si son iguales, >0 si a>b. */
static int vercmp(const char *a, const char *b) {
    if (!a) a = "0";
    if (!b) b = "0";
    while (*a || *b) {
        long na = 0, nb = 0;
        while (*a && *a != '.') { if (*a >= '0' && *a <= '9') na = na * 10 + (*a - '0'); a++; }
        while (*b && *b != '.') { if (*b >= '0' && *b <= '9') nb = nb * 10 + (*b - '0'); b++; }
        if (na != nb) return (na < nb) ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

/* Carga /etc/coco.json y llena g_repos[] */
static void load_config(void) {
    if (g_repo_count > 0) return; /* ya cargado */

    char *buf = read_file(COCO_CONFIG);
    if (!buf) {
        /* default si no existe config: raíz del repo (sin subcarpeta) */
        snprintf(g_repos[0], MAX_URL, "https://repo.coconutdynamics.com");
        g_repo_count = 1;
        return;
    }

    json_error_t err;
    json_t *root = json_loads(buf, 0, &err);
    free(buf);

    if (!root) {
        fprintf(stderr, "[!] Config inválido (%s). Usando repo por defecto.\n", err.text);
        snprintf(g_repos[0], MAX_URL, "https://repo.coconutdynamics.com");
        g_repo_count = 1;
        return;
    }

    json_t *arr = json_object_get(root, "repositories");
    if (json_is_array(arr)) {
        size_t i;
        json_t *v;
        json_array_foreach(arr, i, v) {
            if (i >= MAX_REPOS) break;
            const char *url = json_string_value(v);
            if (url) snprintf(g_repos[g_repo_count++], MAX_URL, "%s", url);
        }
    }
    json_decref(root);
}

/* Abre (o crea) la base de datos SQLite con el schema de coco */
static sqlite3 *open_db(void) {
    sqlite3 *db;
    if (sqlite3_open(COCO_DB, &db) != SQLITE_OK) {
        fprintf(stderr, "[!] No se pudo abrir la BD: %s\n", sqlite3_errmsg(db));
        return NULL;
    }

    const char *schema =
        "CREATE TABLE IF NOT EXISTS installed ("
        "  name        TEXT PRIMARY KEY,"
        "  version     TEXT NOT NULL,"
        "  description TEXT,"
        "  repo        TEXT,"
        "  installed_at TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS files ("
        "  id      INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  package TEXT NOT NULL,"
        "  path    TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS depends ("
        "  package TEXT NOT NULL,"
        "  dep     TEXT NOT NULL"
        ");";

    char *errmsg = NULL;
    sqlite3_exec(db, schema, NULL, NULL, &errmsg);
    if (errmsg) { fprintf(stderr, "[!] Schema error: %s\n", errmsg); sqlite3_free(errmsg); }
    return db;
}

/* Calcula SHA256 de un archivo usando sha256sum del sistema */
static int sha256_file(const char *path, char out[65]) {
    char cmd[768];
    snprintf(cmd, sizeof(cmd), "sha256sum '%s' 2>/dev/null", path);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    int ok = (fscanf(p, "%64s", out) == 1) ? 0 : -1;
    pclose(p);
    return ok;
}

/* Timestamp simple para logs */
static void timestamp(char out[32]) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(out, 32, "%Y-%m-%d %H:%M:%S", tm);
}

/* Extrae el tarball hacia / y registra cada archivo en la DB.
 * También escribe la lista de archivos al log. */
static int extraer_y_registrar(const char *ruta, sqlite3 *db,
                                const char *pkg_name, FILE *log) {
    struct archive *a   = archive_read_new();
    struct archive *ext = archive_write_disk_new();
    struct archive_entry *entry;

    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    int flags = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                ARCHIVE_EXTRACT_ACL  | ARCHIVE_EXTRACT_FFLAGS;
    archive_write_disk_set_options(ext, flags);
    archive_write_disk_set_standard_lookup(ext);

    if (archive_read_open_filename(a, ruta, 10240) != ARCHIVE_OK) {
        fprintf(stderr, "[!] Error abriendo archivo: %s\n", archive_error_string(a));
        archive_read_free(a);
        archive_write_free(ext);
        return -1;
    }

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO files(package, path) VALUES(?, ?);",
        -1, &stmt, NULL);

    for (;;) {
        int r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF) break;
        if (r != ARCHIVE_OK) break;

        const char *path = archive_entry_pathname(entry);
        archive_write_header(ext, entry);

        const void *buff; size_t size; int64_t offset;
        while (archive_read_data_block(a, &buff, &size, &offset) == ARCHIVE_OK)
            archive_write_data_block(ext, buff, size, offset);
        archive_write_finish_entry(ext);

        /* Registrar en DB y en log */
        if (stmt) {
            sqlite3_bind_text(stmt, 1, pkg_name, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, path,     -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_reset(stmt);
        }
        if (log) fprintf(log, "  %s\n", path);
    }

    if (stmt) sqlite3_finalize(stmt);
    archive_read_free(a);
    archive_write_free(ext);
    return 0;
}

/* ── Busca en el índice local el repo y la versión disponible de un paquete ── */
static void lookup_in_index(json_t *idx, const char *pkg_name,
                             const char **out_repo, const char **out_version) {
    *out_repo = NULL;
    *out_version = NULL;
    if (!idx) return;
    json_t *entry = json_object_get(idx, pkg_name);
    if (!entry) return;
    *out_repo    = json_string_value(json_object_get(entry, "_repo"));
    *out_version = json_string_value(json_object_get(entry, "version"));
}

/* Descarga manifest.json + el paquete comprimido desde <repo>/<pkg>/,
 * y verifica el SHA256 contra lo declarado en el manifiesto.
 *
 * Estructura esperada en el repo:
 *   <repo>/<pkg_name>/manifest.json
 *   <repo>/<pkg_name>/<source>        (el .tar.gz, el nombre viene del manifest)
 *
 * pkg_path_out se llena con la ruta local del .tar.gz descargado.
 * Retorna el json_t* del manifiesto (el caller debe json_decref), o NULL si falló
 * (el error ya se imprimió). */
static json_t *fetch_and_verify(const char *pkg_name, const char *pkg_repo,
                                 char *pkg_path_out, size_t pkg_path_sz) {
    char manifest_url[MAX_URL + 160], manifest_path[512];
    snprintf(manifest_url,  sizeof(manifest_url),  "%s/%s/manifest.json", pkg_repo, pkg_name);
    snprintf(manifest_path, sizeof(manifest_path), "%s/%s.manifest.json", COCO_CACHE, pkg_name);

    printf("  -> Descargando manifiesto...\n");
    if (coco_download(manifest_url, manifest_path) != 0) {
        fprintf(stderr, "[!] No se pudo descargar %s\n", manifest_url);
        return NULL;
    }

    char *mbuf = read_file(manifest_path);
    if (!mbuf) { fprintf(stderr, "[!] No se pudo leer el manifiesto\n"); return NULL; }
    json_error_t jerr;
    json_t *manifest = json_loads(mbuf, 0, &jerr);
    free(mbuf);
    if (!manifest) { fprintf(stderr, "[!] JSON inválido: %s\n", jerr.text); return NULL; }

    const char *version    = json_string_value(json_object_get(manifest, "version"));
    const char *source     = json_string_value(json_object_get(manifest, "source"));
    const char *sha256_exp = json_string_value(json_object_get(manifest, "sha256"));

    if (!version || !source) {
        fprintf(stderr, "[!] Manifiesto incompleto (falta 'version' o 'source')\n");
        json_decref(manifest);
        return NULL;
    }

    char pkg_url[MAX_URL + 256];
    snprintf(pkg_url, sizeof(pkg_url), "%s/%s/%s", pkg_repo, pkg_name, source);
    snprintf(pkg_path_out, pkg_path_sz, "%s/%s", COCO_CACHE, source);

    printf("  -> Descargando %s v%s...\n", pkg_name, version);
    if (coco_download(pkg_url, pkg_path_out) != 0) {
        fprintf(stderr, "[!] Error descargando %s\n", pkg_url);
        json_decref(manifest);
        return NULL;
    }

    if (sha256_exp && strlen(sha256_exp) == 64) {
        printf("  -> Verificando SHA256...\n");
        char computed[65] = {0};
        if (sha256_file(pkg_path_out, computed) != 0 || strcmp(computed, sha256_exp) != 0) {
            fprintf(stderr, "\n[!] FALLO DE INTEGRIDAD: SHA256 no coincide para '%s'\n", pkg_name);
            fprintf(stderr, "    Esperado:  %s\n", sha256_exp);
            fprintf(stderr, "    Calculado: %s\n", computed);
            unlink(pkg_path_out);
            json_decref(manifest);
            return NULL;
        }
        printf("  -> Checksum OK \xE2\x9C\x93\n");
    } else {
        printf("  [!] Sin SHA256 en manifiesto, saltando verificación.\n");
    }

    return manifest;
}

/* Registra en SQLite + extrae + corre hooks, a partir de un manifiesto ya
 * descargado y verificado. Se usa tanto para 'install' como para 'upgrade'. */
static int install_from_manifest(const char *pkg_name, const char *pkg_repo,
                                  const char *pkg_path, json_t *manifest,
                                  const char *accion /* "Instalado" o "Actualizado" */) {
    const char *version     = json_string_value(json_object_get(manifest, "version"));
    const char *description = json_string_value(json_object_get(manifest, "description"));
    const char *sha256_exp  = json_string_value(json_object_get(manifest, "sha256"));
    json_t *deps  = json_object_get(manifest, "dependencies");
    json_t *hooks = json_object_get(manifest, "hooks");

    if (json_is_array(deps) && json_array_size(deps) > 0) {
        printf("  -> Dependencias: ");
        size_t i; json_t *dep;
        json_array_foreach(deps, i, dep) printf("%s ", json_string_value(dep));
        printf("\n");
    }

    sqlite3 *db = open_db();
    if (!db) return -1;

    /* Limpia el registro de archivos previo (importante en upgrades,
     * para no dejar basura de versiones anteriores con distinto layout) */
    sqlite3_stmt *clr;
    sqlite3_prepare_v2(db, "DELETE FROM files WHERE package = ?;", -1, &clr, NULL);
    sqlite3_bind_text(clr, 1, pkg_name, -1, SQLITE_STATIC);
    sqlite3_step(clr);
    sqlite3_finalize(clr);

    char log_path[512], ts[32];
    snprintf(log_path, sizeof(log_path), "%s/%s.log", COCO_LOG_DIR, pkg_name);
    FILE *log = fopen(log_path, "w");
    timestamp(ts);
    if (log) {
        fprintf(log, "=== Coco: %s %s v%s ===\n", accion, pkg_name, version);
        fprintf(log, "Fecha:  %s\n", ts);
        fprintf(log, "Repo:   %s\n", pkg_repo);
        fprintf(log, "SHA256: %s\n\n", sha256_exp ? sha256_exp : "N/A");
        fprintf(log, "Archivos instalados:\n");
    }

    printf("  -> Extrayendo paquete...\n");
    if (extraer_y_registrar(pkg_path, db, pkg_name, log) != 0) {
        fprintf(stderr, "[!] Error en la extracción\n");
        if (log) fclose(log);
        sqlite3_close(db);
        return -1;
    }
    if (log) fclose(log);

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO installed(name, version, description, repo, installed_at)"
        " VALUES(?, ?, ?, ?, ?);",
        -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, pkg_name,                        -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, version,                         -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, description ? description : "",  -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, pkg_repo,                        -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, ts,                              -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* Dependencias: reset + reinsert */
    sqlite3_prepare_v2(db, "DELETE FROM depends WHERE package = ?;", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, pkg_name, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (json_is_array(deps) && json_array_size(deps) > 0) {
        size_t i; json_t *dep;
        sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO depends(package, dep) VALUES(?, ?);",
            -1, &stmt, NULL);
        json_array_foreach(deps, i, dep) {
            const char *dname = json_string_value(dep);
            if (!dname) continue;
            sqlite3_bind_text(stmt, 1, pkg_name, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, dname,    -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_reset(stmt);
        }
        sqlite3_finalize(stmt);
    }

    if (json_is_object(hooks)) {
        const char *post = json_string_value(json_object_get(hooks, "post_install"));
        if (post && *post) {
            printf("  -> Hook post-instalación: %s\n", post);
            int ret = system(post);
            if (ret != 0) printf("  [!] Hook retornó código %d\n", ret);
        }
    }

    printf("[+] %s: %s v%s\n", accion, pkg_name, version);
    printf("    Log en: %s\n", log_path);

    sqlite3_close(db);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * COMANDOS PÚBLICOS
 * ═══════════════════════════════════════════════════════════════════════ */

/* coco sync ─ Descarga index.json de cada repo y los fusiona.
 * Si NINGÚN repo responde, conserva el índice anterior en vez de borrarlo. */
int cmd_sync(void) {
    load_config();
    init_dirs();

    printf("[*] Sincronizando repositorios...\n");

    json_t *combined = json_object();
    int repos_ok = 0;

    for (int i = 0; i < g_repo_count; i++) {
        char url[MAX_URL + 64], tmp[512];
        snprintf(url, sizeof(url), "%s/index.json", g_repos[i]);
        snprintf(tmp, sizeof(tmp), "%s/tmp_idx_%d.json", COCO_CACHE, i);

        printf("  -> %s\n", g_repos[i]);
        if (coco_download(url, tmp) != 0) {
            fprintf(stderr, "     [!] No se pudo descargar el índice\n");
            continue;
        }

        char *buf = read_file(tmp);
        if (!buf) continue;

        json_error_t err;
        json_t *idx = json_loads(buf, 0, &err);
        free(buf);
        unlink(tmp);

        if (!idx) { fprintf(stderr, "     [!] JSON inválido\n"); continue; }

        const char *key; json_t *val;
        json_object_foreach(idx, key, val) {
            json_object_set_new(val, "_repo", json_string(g_repos[i]));
            json_object_set(combined, key, val);
        }
        json_decref(idx);
        repos_ok++;
    }

    if (json_object_size(combined) == 0) {
        fprintf(stderr, "[!] No se pudo sincronizar ningún repositorio. Se conserva el índice anterior.\n");
        json_decref(combined);
        return -1;
    }

    int rc = 0;
    if (json_dump_file(combined, COCO_INDEX, JSON_INDENT(2)) == 0) {
        printf("[+] Sync completo. Repos OK: %d/%d. Paquetes disponibles: %zu\n",
               repos_ok, g_repo_count, json_object_size(combined));
    } else {
        fprintf(stderr, "[!] Error guardando el índice local\n");
        rc = -1;
    }

    json_decref(combined);
    return rc;
}

/* coco install <pkg> ─ Descarga, verifica, extrae y registra.
 * Si ya está instalado en la última versión, no hace nada.
 * Si está instalado en una versión vieja, actualiza in-place. */
int cmd_install(const char *pkg_name) {
    load_config();
    init_dirs();
    printf("[*] Instalando: %s\n", pkg_name);

    char *idxbuf = read_file(COCO_INDEX);
    json_t *idx = NULL;
    if (idxbuf) {
        json_error_t e;
        idx = json_loads(idxbuf, 0, &e);
        free(idxbuf);
    } else {
        fprintf(stderr, "[!] Sin índice local. Ejecuta 'coco sync' primero.\n");
    }

    const char *pkg_repo, *avail_version;
    lookup_in_index(idx, pkg_name, &pkg_repo, &avail_version);
    if (!pkg_repo) pkg_repo = g_repos[0]; /* fallback al primer repo */

    /* ¿Ya está instalado? */
    sqlite3 *chk_db = open_db();
    char installed_version[64] = {0};
    int already = 0;
    if (chk_db) {
        sqlite3_stmt *st;
        sqlite3_prepare_v2(chk_db, "SELECT version FROM installed WHERE name = ?;", -1, &st, NULL);
        sqlite3_bind_text(st, 1, pkg_name, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            already = 1;
            snprintf(installed_version, sizeof(installed_version), "%s",
                      (const char *)sqlite3_column_text(st, 0));
        }
        sqlite3_finalize(st);
        sqlite3_close(chk_db);
    }

    if (already && avail_version && vercmp(installed_version, avail_version) >= 0) {
        printf("[+] '%s' ya está instalado en su versión más reciente (v%s).\n",
               pkg_name, installed_version);
        if (idx) json_decref(idx);
        return 0;
    }
    if (already) {
        printf("  -> Versión instalada: v%s. Actualizando a v%s...\n",
               installed_version, avail_version ? avail_version : "?");
    }

    char pkg_path[512];
    json_t *manifest = fetch_and_verify(pkg_name, pkg_repo, pkg_path, sizeof(pkg_path));
    if (idx) json_decref(idx);
    if (!manifest) return -1;

    int rc = install_from_manifest(pkg_name, pkg_repo, pkg_path, manifest,
                                    already ? "Actualizado" : "Instalado");
    json_decref(manifest);
    return rc;
}

/* coco upgrade [pkg] ─ Actualiza un paquete específico, o todos si pkg==NULL.
 * Compara versión instalada vs versión en el índice local (correr 'coco sync' antes). */
int cmd_upgrade(const char *pkg_name) {
    load_config();
    init_dirs();

    char *idxbuf = read_file(COCO_INDEX);
    if (!idxbuf) {
        fprintf(stderr, "[!] Sin índice local. Ejecuta 'coco sync' primero.\n");
        return -1;
    }
    json_error_t e;
    json_t *idx = json_loads(idxbuf, 0, &e);
    free(idxbuf);
    if (!idx) { fprintf(stderr, "[!] Índice corrupto.\n"); return -1; }

    sqlite3 *db = open_db();
    if (!db) { json_decref(idx); return -1; }

    typedef struct { char name[128]; char from[64]; char to[64]; } Upg;
    Upg *list = NULL;
    int count = 0, cap = 0;

    sqlite3_stmt *st;
    if (pkg_name) {
        sqlite3_prepare_v2(db, "SELECT name, version FROM installed WHERE name = ?;", -1, &st, NULL);
        sqlite3_bind_text(st, 1, pkg_name, -1, SQLITE_STATIC);
    } else {
        sqlite3_prepare_v2(db, "SELECT name, version FROM installed ORDER BY name;", -1, &st, NULL);
    }

    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 0);
        const char *cur  = (const char *)sqlite3_column_text(st, 1);
        json_t *entry = json_object_get(idx, name);
        const char *avail = entry ? json_string_value(json_object_get(entry, "version")) : NULL;

        if (avail && vercmp(cur, avail) < 0) {
            if (count == cap) {
                cap = cap ? cap * 2 : 8;
                list = realloc(list, cap * sizeof(Upg));
            }
            snprintf(list[count].name, sizeof(list[count].name), "%s", name);
            snprintf(list[count].from, sizeof(list[count].from), "%s", cur);
            snprintf(list[count].to,   sizeof(list[count].to),   "%s", avail);
            count++;
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);

    if (pkg_name && count == 0) {
        printf("[+] '%s' ya está en su versión más reciente (o no está instalado / no existe en el índice).\n", pkg_name);
        json_decref(idx);
        free(list);
        return 0;
    }
    if (!pkg_name && count == 0) {
        printf("[+] Todo está actualizado. No hay paquetes pendientes.\n");
        json_decref(idx);
        free(list);
        return 0;
    }

    printf("[*] Actualizaciones disponibles (%d):\n", count);
    for (int i = 0; i < count; i++)
        printf("    %-20s v%s -> v%s\n", list[i].name, list[i].from, list[i].to);
    printf("\n");

    int failed = 0;
    for (int i = 0; i < count; i++) {
        printf("[*] Actualizando: %s\n", list[i].name);

        const char *pkg_repo = g_repos[0];
        json_t *entry = json_object_get(idx, list[i].name);
        if (entry) {
            const char *r = json_string_value(json_object_get(entry, "_repo"));
            if (r) pkg_repo = r;
        }

        char pkg_path[512];
        json_t *manifest = fetch_and_verify(list[i].name, pkg_repo, pkg_path, sizeof(pkg_path));
        if (!manifest) { failed++; printf("\n"); continue; }

        if (install_from_manifest(list[i].name, pkg_repo, pkg_path, manifest, "Actualizado") != 0)
            failed++;
        json_decref(manifest);
        printf("\n");
    }

    printf("[+] Actualización completa: %d ok, %d con errores.\n", count - failed, failed);

    json_decref(idx);
    free(list);
    return failed ? -1 : 0;
}

/* coco remove <pkg> ─ Elimina archivos y borra de la DB */
int cmd_remove(const char *pkg_name) {
    sqlite3 *db = open_db();
    if (!db) return -1;

    sqlite3_stmt *chk;
    sqlite3_prepare_v2(db, "SELECT name FROM installed WHERE name = ?;", -1, &chk, NULL);
    sqlite3_bind_text(chk, 1, pkg_name, -1, SQLITE_STATIC);
    int found = (sqlite3_step(chk) == SQLITE_ROW);
    sqlite3_finalize(chk);

    if (!found) {
        fprintf(stderr, "[!] '%s' no está instalado.\n", pkg_name);
        sqlite3_close(db);
        return -1;
    }

    /* Verificar que ningún paquete instalado depende de este */
    sqlite3_stmt *rdep;
    sqlite3_prepare_v2(db,
        "SELECT DISTINCT package FROM depends WHERE dep = ?;",
        -1, &rdep, NULL);
    sqlite3_bind_text(rdep, 1, pkg_name, -1, SQLITE_STATIC);

    int blocked = 0;
    while (sqlite3_step(rdep) == SQLITE_ROW) {
        const char *blocker = (const char *)sqlite3_column_text(rdep, 0);
        if (!blocked) fprintf(stderr, "[!] No se puede eliminar '%s'. Lo requieren:\n", pkg_name);
        fprintf(stderr, "    * %s\n", blocker);
        blocked++;
    }
    sqlite3_finalize(rdep);

    if (blocked) { sqlite3_close(db); return -1; }

    printf("[*] Eliminando: %s\n", pkg_name);

    sqlite3_stmt *files;
    sqlite3_prepare_v2(db, "SELECT path FROM files WHERE package = ?;", -1, &files, NULL);
    sqlite3_bind_text(files, 1, pkg_name, -1, SQLITE_STATIC);

    int removed = 0;
    while (sqlite3_step(files) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(files, 0);
        if (path && unlink(path) == 0) removed++;
    }
    sqlite3_finalize(files);

    sqlite3_stmt *del;

    sqlite3_prepare_v2(db, "DELETE FROM files   WHERE package = ?;", -1, &del, NULL);
    sqlite3_bind_text(del, 1, pkg_name, -1, SQLITE_STATIC);
    sqlite3_step(del); sqlite3_finalize(del);

    sqlite3_prepare_v2(db, "DELETE FROM depends WHERE package = ?;", -1, &del, NULL);
    sqlite3_bind_text(del, 1, pkg_name, -1, SQLITE_STATIC);
    sqlite3_step(del); sqlite3_finalize(del);

    sqlite3_prepare_v2(db, "DELETE FROM installed WHERE name = ?;", -1, &del, NULL);
    sqlite3_bind_text(del, 1, pkg_name, -1, SQLITE_STATIC);
    sqlite3_step(del); sqlite3_finalize(del);

    /* Limpiar cache local del paquete */
    char cache_glob[512];
    snprintf(cache_glob, sizeof(cache_glob), "rm -f '%s/%s'.* 2>/dev/null", COCO_CACHE, pkg_name);
    if (system(cache_glob) != 0) { /* limpieza de caché es best-effort, no es fatal */ }

    printf("[+] '%s' eliminado (%d archivos borrados).\n", pkg_name, removed);
    sqlite3_close(db);
    return 0;
}

/* coco search <term> ─ Busca en el índice local */
int cmd_search(const char *term) {
    if (access(COCO_INDEX, F_OK) != 0) {
        fprintf(stderr, "[!] Sin índice. Ejecuta 'coco sync' primero.\n");
        return -1;
    }

    char *buf = read_file(COCO_INDEX);
    if (!buf) return -1;
    json_error_t e;
    json_t *idx = json_loads(buf, 0, &e);
    free(buf);
    if (!idx) { fprintf(stderr, "[!] Índice corrupto\n"); return -1; }

    printf("%-22s %-10s %s\n", "PAQUETE", "VERSIÓN", "DESCRIPCIÓN");
    printf("──────────────────────────────────────────────────────────────\n");

    int found = 0;
    const char *key; json_t *val;
    json_object_foreach(idx, key, val) {
        const char *desc = json_string_value(json_object_get(val, "description"));
        const char *ver  = json_string_value(json_object_get(val, "version"));
        if (!desc) desc = "";
        if (!ver)  ver  = "?";
        if (strstr(key, term) || strstr(desc, term)) {
            printf("%-22s %-10s %s\n", key, ver, desc);
            found++;
        }
    }

    if (!found) printf("Sin resultados para '%s'\n", term);
    json_decref(idx);
    return 0;
}

/* coco list ─ Lista paquetes instalados desde SQLite.
 * Si hay índice local, marca los que tienen actualización disponible. */
int cmd_list(void) {
    sqlite3 *db = open_db();
    if (!db) return -1;

    char *idxbuf = read_file(COCO_INDEX);
    json_t *idx = NULL;
    if (idxbuf) {
        json_error_t e;
        idx = json_loads(idxbuf, 0, &e);
        free(idxbuf);
    }

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db,
        "SELECT name, version, installed_at FROM installed ORDER BY name;",
        -1, &stmt, NULL);

    printf("%-22s %-12s %-20s %s\n", "PAQUETE", "VERSIÓN", "INSTALADO", "");
    printf("────────────────────────────────────────────────────────────────────────\n");

    int count = 0, upgradable = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        const char *ver  = (const char *)sqlite3_column_text(stmt, 1);
        const char *date = (const char *)sqlite3_column_text(stmt, 2);

        const char *tag = "";
        if (idx) {
            json_t *entry = json_object_get(idx, name);
            const char *avail = entry ? json_string_value(json_object_get(entry, "version")) : NULL;
            if (avail && vercmp(ver, avail) < 0) { tag = "(actualización disponible)"; upgradable++; }
        }

        printf("%-22s %-12s %-20s %s\n", name, ver, date ? date : "?", tag);
        count++;
    }

    if (count == 0) printf("No hay paquetes instalados.\n");
    else {
        printf("\n%d paquete(s) instalado(s)", count);
        if (upgradable) printf(", %d con actualización disponible (coco upgrade)", upgradable);
        printf(".\n");
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    if (idx) json_decref(idx);
    return 0;
}

/* coco info <pkg> ─ Muestra info del índice y estado de instalación */
int cmd_info(const char *pkg_name) {
    char *buf = read_file(COCO_INDEX);
    if (!buf) { fprintf(stderr, "[!] Sin índice. Ejecuta 'coco sync' primero.\n"); return -1; }
    json_error_t e;
    json_t *idx = json_loads(buf, 0, &e);
    free(buf);

    if (!idx) { fprintf(stderr, "[!] Índice corrupto\n"); return -1; }

    json_t *pkg = json_object_get(idx, pkg_name);
    if (!pkg) {
        printf("[!] '%s' no encontrado en el índice.\n", pkg_name);
        json_decref(idx);
        return -1;
    }

    const char *ver  = json_string_value(json_object_get(pkg, "version"));
    const char *desc = json_string_value(json_object_get(pkg, "description"));
    const char *repo = json_string_value(json_object_get(pkg, "_repo"));

    printf("Nombre:      %s\n", pkg_name);
    printf("Versión:     %s\n", ver  ? ver  : "?");
    printf("Descripción: %s\n", desc ? desc : "?");
    printf("Repo:        %s\n", repo ? repo : "?");

    sqlite3 *db = open_db();
    if (db) {
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db,
            "SELECT version, installed_at FROM installed WHERE name = ?;",
            -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, pkg_name, -1, SQLITE_STATIC);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *inst_ver = (const char *)sqlite3_column_text(stmt, 0);
            printf("Estado:      Instalado (v%s el %s)\n",
                inst_ver, (const char *)sqlite3_column_text(stmt, 1));

            if (ver && vercmp(inst_ver, ver) < 0)
                printf("Actualización disponible: v%s -> v%s  (coco upgrade %s)\n", inst_ver, ver, pkg_name);

            sqlite3_stmt *fc;
            sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM files WHERE package = ?;", -1, &fc, NULL);
            sqlite3_bind_text(fc, 1, pkg_name, -1, SQLITE_STATIC);
            if (sqlite3_step(fc) == SQLITE_ROW)
                printf("Archivos:    %d\n", sqlite3_column_int(fc, 0));
            sqlite3_finalize(fc);
        } else {
            printf("Estado:      No instalado\n");
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }

    json_decref(idx);
    return 0;
}

/* coco log <pkg> ─ Muestra el log de instalación con less */
int cmd_log(const char *pkg_name) {
    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/%s.log", COCO_LOG_DIR, pkg_name);

    if (access(log_path, F_OK) != 0) {
        fprintf(stderr, "[!] No hay log para '%s' en %s\n", pkg_name, log_path);
        return -1;
    }

    char cmd[768];
    snprintf(cmd, sizeof(cmd), "less '%s'", log_path);
    return system(cmd) == 0 ? 0 : -1;
}
