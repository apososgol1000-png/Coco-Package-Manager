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
#include "solver.h"
#include "gpgverify.h"
#include "depspec.h"

/* ─── Rutas ─────────────────────────────────────────────────────────── */
#define COCO_CACHE    "/var/coco/cache"
#define COCO_DB       "/var/coco/db/coco.db"
#define COCO_INDEX    "/var/coco/db/index.json"
#define COCO_LOG_DIR  "/var/log/coco"
#define COCO_CONFIG   "/etc/coco.json"
#define COCO_HIST_DIR "/var/coco/history"

#define MAX_REPOS  8
#define MAX_URL    512

/* ─── Config global ─────────────────────────────────────────────────── */
static char g_repos[MAX_REPOS][MAX_URL];
static int  g_repo_count = 0;
static char g_keyring[MAX_URL];
static int  g_has_keyring = 0;

/* ═══════════════════════════════════════════════════════════════════════
 * HELPERS INTERNOS
 * ═══════════════════════════════════════════════════════════════════════ */

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
    mkdir_p(COCO_HIST_DIR);
}

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

static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[8192]; size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    }
    fclose(in); fclose(out);
    return ok ? 0 : -1;
}

/* Compara versiones tipo "1.2.10" vs "1.9.0" numéricamente. */
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

/* Carga /etc/coco.json: repos[] y, si existe, el llavero GPG de confianza. */
static void load_config(void) {
    if (g_repo_count > 0) return;

    char *buf = read_file(COCO_CONFIG);
    if (!buf) {
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
        size_t i; json_t *v;
        json_array_foreach(arr, i, v) {
            if (i >= MAX_REPOS) break;
            const char *url = json_string_value(v);
            if (url) snprintf(g_repos[g_repo_count++], MAX_URL, "%s", url);
        }
    }

    const char *kr = json_string_value(json_object_get(root, "keyring"));
    if (kr && access(kr, F_OK) == 0) {
        snprintf(g_keyring, sizeof(g_keyring), "%s", kr);
        g_has_keyring = 1;
    }

    json_decref(root);
}

/* Agrega `column` a `table` si todavía no existe (para BDs creadas antes de
 * v0.5). Retorna 1 si tuvo que migrar, 0 si la columna ya estaba. */
static int ensure_column(sqlite3 *db, const char *table, const char *column, const char *coldef) {
    char probe[256];
    snprintf(probe, sizeof(probe), "SELECT %s FROM %s LIMIT 0;", column, table);
    char *err = NULL;
    int rc = sqlite3_exec(db, probe, NULL, NULL, &err);
    if (err) { sqlite3_free(err); err = NULL; }
    if (rc == SQLITE_OK) return 0; /* ya existe */

    char alter[256];
    snprintf(alter, sizeof(alter), "ALTER TABLE %s ADD COLUMN %s %s;", table, column, coldef);
    sqlite3_exec(db, alter, NULL, NULL, &err);
    if (err) {
        fprintf(stderr, "[!] Migración de esquema (%s.%s) falló: %s\n", table, column, err);
        sqlite3_free(err);
    }
    return 1;
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
        "  installed_at TEXT,"
        "  explicit    INTEGER NOT NULL DEFAULT 1" /* 1 = pedido directamente por el usuario, 0 = instalado solo como dependencia (ver autoremove) */
        ");"
        "CREATE TABLE IF NOT EXISTS files ("
        "  id      INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  package TEXT NOT NULL,"
        "  path    TEXT NOT NULL"
        ");"
        /* depends/conflicts/provides comparten forma: `dep` es el spec crudo
         * tal cual viene del manifest ("curl" o "curl>=1.2.0"), que es lo que
         * solver.c necesita para construir la relación versionada en libsolv.
         * `dep_name` es solo el nombre ya separado del operador -- para poder
         * hacer WHERE dep_name = 'curl' exacto sin importar qué restricción
         * de versión traiga (usado por remove/autoremove). */
        "CREATE TABLE IF NOT EXISTS depends ("
        "  package  TEXT NOT NULL,"
        "  dep      TEXT NOT NULL,"
        "  dep_name TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS conflicts ("
        "  package  TEXT NOT NULL,"
        "  dep      TEXT NOT NULL,"
        "  dep_name TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS provides ("
        "  package  TEXT NOT NULL,"
        "  dep      TEXT NOT NULL,"
        "  dep_name TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS config_files ("
        "  package TEXT NOT NULL,"
        "  path    TEXT NOT NULL,"
        "  sha256  TEXT NOT NULL,"
        "  PRIMARY KEY (package, path)"
        ");"
        "CREATE TABLE IF NOT EXISTS history ("
        "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  package       TEXT NOT NULL,"
        "  version       TEXT NOT NULL,"
        "  manifest_path TEXT NOT NULL,"
        "  pkg_path      TEXT NOT NULL,"
        "  ts            TEXT NOT NULL"
        ");";

    char *errmsg = NULL;
    sqlite3_exec(db, schema, NULL, NULL, &errmsg);
    if (errmsg) { fprintf(stderr, "[!] Schema error: %s\n", errmsg); sqlite3_free(errmsg); }

    /* Migraciones para BDs creadas con una v0.4 o anterior */
    ensure_column(db, "installed", "explicit", "INTEGER NOT NULL DEFAULT 1");
    if (ensure_column(db, "depends", "dep_name", "TEXT NOT NULL DEFAULT ''")) {
        /* Las filas viejas nunca tuvieron operador de versión, así que el
         * nombre plano es exactamente lo que ya había en `dep`. */
        sqlite3_exec(db, "UPDATE depends SET dep_name = dep WHERE dep_name = '';", NULL, NULL, NULL);
    }

    return db;
}

/* Lee el flag `explicit` de un paquete ya instalado. Si no está instalado
 * (llamada previa a la primera instalación), asume explícito por defecto:
 * es la postura segura para no perder paquetes en autoremove por error. */
static int get_explicit_flag(sqlite3 *db, const char *name) {
    sqlite3_stmt *st;
    int val = 1;
    sqlite3_prepare_v2(db, "SELECT explicit FROM installed WHERE name = ?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) val = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return val;
}

static int sha256_file(const char *path, char out[65]) {
    char cmd[768];
    snprintf(cmd, sizeof(cmd), "sha256sum '%s' 2>/dev/null", path);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    int ok = (fscanf(p, "%64s", out) == 1) ? 0 : -1;
    pclose(p);
    return ok;
}

static void timestamp(char out[32]) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(out, 32, "%Y-%m-%d %H:%M:%S", tm);
}

/* Extrae el tarball hacia / y registra cada archivo en la DB.
 * Si config_files_arr trae rutas que el usuario modificó desde la última
 * instalación (hash distinto al guardado), NO las pisa: escribe la versión
 * nueva como "<ruta>.coco-new" y avisa, igual que dpkg con sus conffiles. */
static int extraer_y_registrar(const char *ruta, sqlite3 *db,
                                const char *pkg_name, FILE *log,
                                json_t *config_files_arr) {
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
    sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO files(package, path) VALUES(?, ?);", -1, &stmt, NULL);

    sqlite3_stmt *cfg_get = NULL, *cfg_set = NULL;
    sqlite3_prepare_v2(db, "SELECT sha256 FROM config_files WHERE package = ? AND path = ?;", -1, &cfg_get, NULL);
    sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO config_files(package, path, sha256) VALUES(?, ?, ?);", -1, &cfg_set, NULL);

    for (;;) {
        int r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF) break;
        if (r != ARCHIVE_OK) break;

        const char *raw_path = archive_entry_pathname(entry);
        char abs_path[600];
        snprintf(abs_path, sizeof(abs_path), (raw_path[0] == '/') ? "%s" : "/%s", raw_path);

        int is_config = 0;
        if (config_files_arr && json_is_array(config_files_arr)) {
            size_t ci; json_t *cv;
            json_array_foreach(config_files_arr, ci, cv) {
                const char *cp = json_string_value(cv);
                if (cp && strcmp(cp, abs_path) == 0) { is_config = 1; break; }
            }
        }

        int redirected = 0;
        char redirected_relpath[620];

        if (is_config && access(abs_path, F_OK) == 0) {
            char stored_hash[65] = {0};
            int has_stored = 0;
            sqlite3_bind_text(cfg_get, 1, pkg_name, -1, SQLITE_STATIC);
            sqlite3_bind_text(cfg_get, 2, abs_path, -1, SQLITE_STATIC);
            if (sqlite3_step(cfg_get) == SQLITE_ROW) {
                const char *sh = (const char *)sqlite3_column_text(cfg_get, 0);
                if (sh) { snprintf(stored_hash, sizeof(stored_hash), "%s", sh); has_stored = 1; }
            }
            sqlite3_reset(cfg_get);

            char current_hash[65] = {0};
            sha256_file(abs_path, current_hash);

            if (has_stored && strcmp(stored_hash, current_hash) != 0) {
                redirected = 1;
                snprintf(redirected_relpath, sizeof(redirected_relpath), "%s.coco-new", raw_path);
                archive_entry_set_pathname(entry, redirected_relpath);
                printf("  [!] %s fue modificado manualmente -> nueva versión en %s.coco-new\n", abs_path, abs_path);
                if (log) fprintf(log, "  [CONFIG PROTEGIDO] %s (version nueva en %s.coco-new)\n", abs_path, abs_path);
            }
        }

        archive_write_header(ext, entry);
        const void *buff; size_t size; int64_t offset;
        while (archive_read_data_block(a, &buff, &size, &offset) == ARCHIVE_OK)
            archive_write_data_block(ext, buff, size, offset);
        archive_write_finish_entry(ext);

        const char *stored_relpath = redirected ? redirected_relpath : raw_path;

        if (is_config && !redirected) {
            char new_hash[65] = {0};
            sha256_file(abs_path, new_hash);
            sqlite3_bind_text(cfg_set, 1, pkg_name, -1, SQLITE_STATIC);
            sqlite3_bind_text(cfg_set, 2, abs_path, -1, SQLITE_STATIC);
            sqlite3_bind_text(cfg_set, 3, new_hash, -1, SQLITE_STATIC);
            sqlite3_step(cfg_set);
            sqlite3_reset(cfg_set);
        }

        if (stmt) {
            sqlite3_bind_text(stmt, 1, pkg_name, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, stored_relpath, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_reset(stmt);
        }
        if (log) fprintf(log, "  %s\n", stored_relpath);
    }

    if (stmt)    sqlite3_finalize(stmt);
    if (cfg_get) sqlite3_finalize(cfg_get);
    if (cfg_set) sqlite3_finalize(cfg_set);
    archive_read_free(a);
    archive_write_free(ext);
    return 0;
}

static void lookup_in_index(json_t *idx, const char *pkg_name,
                             const char **out_repo, const char **out_version,
                             const char **out_manifest_sha256) {
    *out_repo = NULL;
    *out_version = NULL;
    *out_manifest_sha256 = NULL;
    if (!idx) return;
    json_t *entry = json_object_get(idx, pkg_name);
    if (!entry) return;
    *out_repo            = json_string_value(json_object_get(entry, "_repo"));
    *out_version          = json_string_value(json_object_get(entry, "version"));
    *out_manifest_sha256 = json_string_value(json_object_get(entry, "manifest_sha256"));
}

/* Un paquete a instalar/actualizar como parte de un lote: lo que junta
 * cmd_install/cmd_upgrade antes de llamar a fetch_and_verify_many(), y lo
 * que queda listo (o en NULL si falló) después. */
typedef struct {
    char name[128];
    const char *repo;              /* vive mientras viva idx / g_repos, no se copia */
    const char *manifest_sha256;   /* idem */
    char manifest_path[512];
    char pkg_path[512];
    json_t *manifest;              /* NULL hasta que se descargue+parsee+verifique bien */
    int already;                   /* 1 = actualización, 0 = instalación nueva */
    int explicit_flag;             /* qué va a quedar en installed.explicit */
} InstallJob;

/* Descarga (en paralelo) el manifest.json de cada job, lo verifica contra
 * el índice firmado si aplica, lo parsea, y con la URL del tarball que ahí
 * declara (`source`) descarga (también en paralelo) los tarballs -- dos
 * rondas porque la URL del segundo archivo solo se conoce tras leer el
 * primero. Verifica SHA256 de ambos igual que hacía el fetch_and_verify()
 * secuencial de antes. jobs[i].manifest queda NULL si ese paquete falló en
 * cualquier paso; los demás jobs del lote siguen su curso normal. */
static void fetch_and_verify_many(InstallJob *jobs, int njobs) {
    if (njobs <= 0) return;

    /* ─── Ronda 1: manifests ─── */
    CocoDownloadJob *dl = calloc((size_t)njobs, sizeof(CocoDownloadJob));
    int *ok = calloc((size_t)njobs, sizeof(int));
    char (*urls)[MAX_URL + 160] = calloc((size_t)njobs, sizeof(*urls));

    for (int i = 0; i < njobs; i++) {
        snprintf(urls[i], sizeof(urls[i]), "%s/%s/manifest.json", jobs[i].repo, jobs[i].name);
        dl[i].url = urls[i];
        dl[i].dest_path = jobs[i].manifest_path;
    }

    printf("[*] Descargando %d manifiesto%s en paralelo...\n", njobs, njobs == 1 ? "" : "s");
    int mok = coco_download_many(dl, njobs, ok);
    printf("    -> %d/%d manifiesto%s descargado%s.\n", mok, njobs, njobs == 1 ? "" : "s", njobs == 1 ? "" : "s");

    for (int i = 0; i < njobs; i++) {
        if (!ok[i]) { fprintf(stderr, "    [!] No se pudo descargar manifest.json de '%s'\n", jobs[i].name); continue; }

        if (jobs[i].manifest_sha256 && strlen(jobs[i].manifest_sha256) == 64) {
            char computed[65] = {0};
            if (sha256_file(jobs[i].manifest_path, computed) != 0 ||
                strcmp(computed, jobs[i].manifest_sha256) != 0) {
                fprintf(stderr, "    [!] Manifiesto de '%s' no coincide con el índice firmado (posible manipulación).\n", jobs[i].name);
                continue;
            }
        }

        char *mbuf = read_file(jobs[i].manifest_path);
        if (!mbuf) { fprintf(stderr, "    [!] No se pudo leer el manifiesto de '%s'\n", jobs[i].name); continue; }
        json_error_t jerr;
        json_t *manifest = json_loads(mbuf, 0, &jerr);
        free(mbuf);
        if (!manifest) { fprintf(stderr, "    [!] JSON inválido en manifiesto de '%s': %s\n", jobs[i].name, jerr.text); continue; }

        const char *version = json_string_value(json_object_get(manifest, "version"));
        const char *source  = json_string_value(json_object_get(manifest, "source"));
        if (!version || !source) {
            fprintf(stderr, "    [!] Manifiesto de '%s' incompleto (falta 'version' o 'source')\n", jobs[i].name);
            json_decref(manifest);
            continue;
        }

        snprintf(jobs[i].pkg_path, sizeof(jobs[i].pkg_path), "%s/%s", COCO_CACHE, source);
        jobs[i].manifest = manifest; /* el SHA256 del tarball se checa hasta la ronda 2 */
    }

    free(dl); free(ok); free(urls);

    /* ─── Ronda 2: tarballs, solo para los jobs cuyo manifest sí sirvió ─── */
    int n2 = 0;
    for (int i = 0; i < njobs; i++) if (jobs[i].manifest) n2++;
    if (n2 == 0) return;

    CocoDownloadJob *dl2   = calloc((size_t)n2, sizeof(CocoDownloadJob));
    char (*urls2)[MAX_URL + 256] = calloc((size_t)n2, sizeof(*urls2));
    int *idx_map = calloc((size_t)n2, sizeof(int)); /* dl2[k] corresponde a jobs[idx_map[k]] */
    int *ok2 = calloc((size_t)n2, sizeof(int));

    int k = 0;
    for (int i = 0; i < njobs; i++) {
        if (!jobs[i].manifest) continue;
        const char *source = json_string_value(json_object_get(jobs[i].manifest, "source"));
        snprintf(urls2[k], sizeof(urls2[k]), "%s/%s/%s", jobs[i].repo, jobs[i].name, source);
        dl2[k].url = urls2[k];
        dl2[k].dest_path = jobs[i].pkg_path;
        idx_map[k] = i;
        k++;
    }

    printf("[*] Descargando %d paquete%s en paralelo...\n", n2, n2 == 1 ? "" : "s");
    int tok = coco_download_many(dl2, n2, ok2);
    printf("    -> %d/%d paquete%s descargado%s.\n", tok, n2, n2 == 1 ? "" : "s", n2 == 1 ? "" : "s");

    for (int i = 0; i < n2; i++) {
        InstallJob *job = &jobs[idx_map[i]];
        if (!ok2[i]) {
            fprintf(stderr, "    [!] Error descargando el paquete de '%s'\n", job->name);
            json_decref(job->manifest); job->manifest = NULL;
            continue;
        }

        const char *sha256_exp = json_string_value(json_object_get(job->manifest, "sha256"));
        if (sha256_exp && strlen(sha256_exp) == 64) {
            char computed[65] = {0};
            if (sha256_file(job->pkg_path, computed) != 0 || strcmp(computed, sha256_exp) != 0) {
                fprintf(stderr, "    [!] FALLO DE INTEGRIDAD: SHA256 no coincide para '%s'\n", job->name);
                unlink(job->pkg_path);
                json_decref(job->manifest); job->manifest = NULL;
                continue;
            }
        } else {
            printf("    [!] '%s' sin SHA256 en manifiesto, saltando verificación.\n", job->name);
        }
    }

    free(dl2); free(urls2); free(idx_map); free(ok2);
}

/* Antes de pisar una versión instalada, respalda su manifest+tarball
 * (todavía en caché desde que se instaló) a /var/coco/history/, y deja
 * un registro en la tabla history para poder hacer 'coco rollback' después. */
static void save_history_snapshot(sqlite3 *db, const char *pkg_name) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db, "SELECT version FROM installed WHERE name = ?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, pkg_name, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); return; }
    char old_version[64];
    snprintf(old_version, sizeof(old_version), "%s", (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);

    char old_manifest_cache[512];
    snprintf(old_manifest_cache, sizeof(old_manifest_cache), "%s/%s.manifest.json", COCO_CACHE, pkg_name);
    char *mbuf = read_file(old_manifest_cache);
    if (!mbuf) return; /* no había caché de la versión anterior, no se puede respaldar */

    json_error_t e;
    json_t *old_manifest = json_loads(mbuf, 0, &e);
    free(mbuf);
    if (!old_manifest) return;

    const char *source = json_string_value(json_object_get(old_manifest, "source"));
    if (!source) { json_decref(old_manifest); return; }

    char old_tarball_cache[512];
    snprintf(old_tarball_cache, sizeof(old_tarball_cache), "%s/%s", COCO_CACHE, source);
    if (access(old_tarball_cache, F_OK) != 0) { json_decref(old_manifest); return; }

    char hist_dir[512];
    snprintf(hist_dir, sizeof(hist_dir), "%s/%s/%s", COCO_HIST_DIR, pkg_name, old_version);
    mkdir_p(hist_dir);

    char dst_manifest[560], dst_tarball[560];
    snprintf(dst_manifest, sizeof(dst_manifest), "%s/manifest.json", hist_dir);
    snprintf(dst_tarball,  sizeof(dst_tarball),  "%s/%s", hist_dir, source);

    if (copy_file(old_manifest_cache, dst_manifest) == 0 &&
        copy_file(old_tarball_cache, dst_tarball) == 0) {
        char ts[32]; timestamp(ts);
        sqlite3_stmt *ins;
        sqlite3_prepare_v2(db,
            "INSERT INTO history(package, version, manifest_path, pkg_path, ts) VALUES(?, ?, ?, ?, ?);",
            -1, &ins, NULL);
        sqlite3_bind_text(ins, 1, pkg_name,      -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 2, old_version,   -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 3, dst_manifest,  -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 4, dst_tarball,   -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 5, ts,            -1, SQLITE_STATIC);
        sqlite3_step(ins);
        sqlite3_finalize(ins);
        printf("  -> Respaldo guardado: %s v%s (para 'coco rollback')\n", pkg_name, old_version);
    }
    json_decref(old_manifest);
}

/* Inserta cada spec de `arr` (dependencies/conflicts/provides, strings tipo
 * "curl" o "curl>=1.2.0") en `table` para pkg_name, guardando tanto el
 * spec crudo (columna dep, para el solver) como el nombre ya separado
 * (columna dep_name, para búsquedas exactas en remove/autoremove). */
static void store_spec_table(sqlite3 *db, const char *table, const char *pkg_name, json_t *arr) {
    if (!json_is_array(arr) || json_array_size(arr) == 0) return;
    char sql[128];
    snprintf(sql, sizeof(sql), "INSERT INTO %s(package, dep, dep_name) VALUES(?, ?, ?);", table);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    size_t i; json_t *v;
    json_array_foreach(arr, i, v) {
        const char *raw = json_string_value(v);
        if (!raw || !*raw) continue;
        char name[128];
        if (coco_depspec_parse(raw, name, sizeof(name), NULL, NULL, 0) != 0) continue;
        sqlite3_bind_text(stmt, 1, pkg_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, raw,      -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, name,     -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
}

static void print_spec_list(const char *label, json_t *arr) {
    if (!json_is_array(arr) || json_array_size(arr) == 0) return;
    printf("  -> %s: ", label);
    size_t i; json_t *v;
    json_array_foreach(arr, i, v) printf("%s ", json_string_value(v));
    printf("\n");
}

/* Registra en SQLite + extrae + corre hooks, a partir de un manifiesto ya
 * descargado y verificado. Se usa para 'install', 'upgrade' y 'rollback'.
 *
 * explicit_flag: 1 si el usuario pidió este paquete directamente (el
 * target de 'coco install', o cualquier 'coco upgrade'/'coco rollback'
 * sobre algo que ya era explícito), 0 si se instala solo porque otro
 * paquete lo necesita como dependencia (candidato a 'coco autoremove'
 * el día que ya nadie más lo requiera). */
static int install_from_manifest(const char *pkg_name, const char *pkg_repo,
                                  const char *pkg_path, json_t *manifest,
                                  const char *accion, int explicit_flag) {
    const char *version     = json_string_value(json_object_get(manifest, "version"));
    const char *description = json_string_value(json_object_get(manifest, "description"));
    const char *sha256_exp  = json_string_value(json_object_get(manifest, "sha256"));
    json_t *deps          = json_object_get(manifest, "dependencies");
    json_t *conflicts_arr  = json_object_get(manifest, "conflicts");
    json_t *provides_arr   = json_object_get(manifest, "provides");
    json_t *hooks         = json_object_get(manifest, "hooks");
    json_t *config_files  = json_object_get(manifest, "config_files");

    print_spec_list("Dependencias",       deps);
    print_spec_list("Conflictos",         conflicts_arr);
    print_spec_list("Provee (virtual)",   provides_arr);

    sqlite3 *db = open_db();
    if (!db) return -1;

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
    if (extraer_y_registrar(pkg_path, db, pkg_name, log, config_files) != 0) {
        fprintf(stderr, "[!] Error en la extracción\n");
        if (log) fclose(log);
        sqlite3_close(db);
        return -1;
    }
    if (log) fclose(log);

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO installed(name, version, description, repo, installed_at, explicit)"
        " VALUES(?, ?, ?, ?, ?, ?);",
        -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, pkg_name,                        -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, version,                         -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, description ? description : "",  -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, pkg_repo,                        -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, ts,                              -1, SQLITE_STATIC);
    sqlite3_bind_int (stmt, 6, explicit_flag ? 1 : 0);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    static const char *spec_tables[] = { "depends", "conflicts", "provides" };
    for (size_t t = 0; t < sizeof(spec_tables) / sizeof(spec_tables[0]); t++) {
        char clr_sql[64];
        snprintf(clr_sql, sizeof(clr_sql), "DELETE FROM %s WHERE package = ?;", spec_tables[t]);
        sqlite3_prepare_v2(db, clr_sql, -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, pkg_name, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    store_spec_table(db, "depends",   pkg_name, deps);
    store_spec_table(db, "conflicts", pkg_name, conflicts_arr);
    store_spec_table(db, "provides",  pkg_name, provides_arr);

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

int cmd_sync(void) {
    load_config();
    init_dirs();

    printf("[*] Sincronizando repositorios...\n");
    if (g_has_keyring)
        printf("    (verificación GPG activa: %s)\n", g_keyring);

    json_t *combined = json_object();
    int repos_ok = 0;

    /* Fase 1: bajar el index.json (y su .sig si hay llavero) de TODOS los
     * repos configurados a la vez, en vez de uno por uno. Metemos ambos
     * tipos de archivo en el mismo lote para aprovechar al máximo el
     * paralelismo (con o sin firma, todo sale junto). */
    char (*idx_tmp)[512]     = calloc((size_t)g_repo_count, sizeof(*idx_tmp));
    char (*sig_tmp)[512]     = calloc((size_t)g_repo_count, sizeof(*sig_tmp));
    char (*idx_url)[MAX_URL + 64] = calloc((size_t)g_repo_count, sizeof(*idx_url));
    char (*sig_url)[MAX_URL + 64] = calloc((size_t)g_repo_count, sizeof(*sig_url));

    int njobs = g_repo_count * (g_has_keyring ? 2 : 1);
    CocoDownloadJob *dl = calloc((size_t)njobs, sizeof(CocoDownloadJob));
    int *ok = calloc((size_t)njobs, sizeof(int));
    int j = 0;
    for (int i = 0; i < g_repo_count; i++) {
        snprintf(idx_url[i], sizeof(idx_url[i]), "%s/index.json", g_repos[i]);
        snprintf(idx_tmp[i], sizeof(idx_tmp[i]), "%s/tmp_idx_%d.json", COCO_CACHE, i);
        dl[j].url = idx_url[i]; dl[j].dest_path = idx_tmp[i]; j++;

        if (g_has_keyring) {
            snprintf(sig_url[i], sizeof(sig_url[i]), "%s/index.json.sig", g_repos[i]);
            snprintf(sig_tmp[i], sizeof(sig_tmp[i]), "%s/tmp_idx_%d.json.sig", COCO_CACHE, i);
            dl[j].url = sig_url[i]; dl[j].dest_path = sig_tmp[i]; j++;
        }
    }

    printf("  -> Descargando %d índice%s en paralelo...\n", g_repo_count, g_repo_count == 1 ? "" : "s");
    coco_download_many(dl, njobs, ok);

    /* Fase 2: procesar cada repo EN ORDEN (0..g_repo_count-1), ya con todo
     * en disco -- así el "último repo gana" en claves duplicadas se
     * mantiene igual que antes, sin importar en qué orden terminó la red. */
    for (int i = 0; i < g_repo_count; i++) {
        int idx_ok = ok[i * (g_has_keyring ? 2 : 1)];
        printf("  -> %s\n", g_repos[i]);

        if (!idx_ok) {
            fprintf(stderr, "     [!] No se pudo descargar el índice\n");
            continue;
        }

        if (g_has_keyring) {
            int sig_ok = ok[i * 2 + 1];
            if (!sig_ok) {
                fprintf(stderr, "     [!] Repo sin firma (index.json.sig) -- RECHAZADO\n");
                unlink(idx_tmp[i]);
                continue;
            }
            int v = gpg_verify_detached(idx_tmp[i], sig_tmp[i], g_keyring);
            unlink(sig_tmp[i]);
            if (v != 1) {
                fprintf(stderr, "     [!] FIRMA INVÁLIDA -- repo RECHAZADO (posible manipulación)\n");
                unlink(idx_tmp[i]);
                continue;
            }
            printf("     -> Firma GPG verificada \xE2\x9C\x93\n");
        }

        char *buf = read_file(idx_tmp[i]);
        if (!buf) continue;

        json_error_t err;
        json_t *idx = json_loads(buf, 0, &err);
        free(buf);
        unlink(idx_tmp[i]);

        if (!idx) { fprintf(stderr, "     [!] JSON inválido\n"); continue; }

        const char *key; json_t *val;
        json_object_foreach(idx, key, val) {
            json_object_set_new(val, "_repo", json_string(g_repos[i]));
            json_object_set(combined, key, val);
        }
        json_decref(idx);
        repos_ok++;
    }

    free(idx_tmp); free(sig_tmp); free(idx_url); free(sig_url);
    free(dl); free(ok);

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

/* coco install <pkg> [--dry-run] ─ Resuelve dependencias con libsolv,
 * muestra el plan, y si no es dry-run instala todo en el orden correcto. */
/* is_target: ¿`name` es uno de los paquetes que el usuario pidió
 * directamente (coco install a b c), o llegó solo como dependencia? */
static int is_requested(const char **pkg_names, int npkgs, const char *name) {
    for (int i = 0; i < npkgs; i++)
        if (!strcmp(pkg_names[i], name)) return 1;
    return 0;
}

/* coco install <pkg...> [--dry-run] -- uno o varios paquetes a la vez.
 * Todos se resuelven JUNTOS en una sola transacción de libsolv (así se
 * detectan conflictos entre ellos y se comparten dependencias repetidas),
 * y sus descargas se paralelizan igual que ya hacía con un solo paquete. */
int cmd_install(const char **pkg_names, int npkgs, int dry_run) {
    load_config();
    init_dirs();

    char *idxbuf = read_file(COCO_INDEX);
    json_t *idx = NULL;
    if (idxbuf) {
        json_error_t e;
        idx = json_loads(idxbuf, 0, &e);
        free(idxbuf);
    }
    if (!idx) {
        fprintf(stderr, "[!] Sin índice local. Ejecuta 'coco sync' primero.\n");
        return -1;
    }

    sqlite3 *db = open_db();
    if (!db) { json_decref(idx); return -1; }

    CocoPlanStep *steps = NULL;
    int count = 0;
    if (solver_plan_install(idx, db, pkg_names, npkgs, &steps, &count) != 0) {
        sqlite3_close(db);
        json_decref(idx);
        return -1;
    }

    if (count == 0) {
        /* Ya estaba todo instalado. Los que pidieron a propósito y quedaron
         * marcados como auto-instalados de una dependencia anterior, los
         * promovemos a explícitos -- igual que 'apt install' sobre algo
         * que ya está presente. */
        sqlite3_stmt *up;
        sqlite3_prepare_v2(db, "UPDATE installed SET explicit = 1 WHERE name = ? AND explicit = 0;", -1, &up, NULL);
        for (int i = 0; i < npkgs; i++) {
            sqlite3_bind_text(up, 1, pkg_names[i], -1, SQLITE_STATIC);
            sqlite3_step(up);
            sqlite3_reset(up);
        }
        sqlite3_finalize(up);

        printf("[+] Ya está%s instalado%s junto con todas sus dependencias: ",
               npkgs == 1 ? "" : "n", npkgs == 1 ? "" : "s");
        for (int i = 0; i < npkgs; i++) printf("%s%s", pkg_names[i], i + 1 < npkgs ? ", " : "\n");
        sqlite3_close(db); json_decref(idx); free(steps);
        return 0;
    }

    printf("[*] Plan de instalación para ");
    for (int i = 0; i < npkgs; i++) printf("%s%s", pkg_names[i], i + 1 < npkgs ? ", " : "");
    printf(" (%d paquete%s vía libsolv):\n", count, count == 1 ? "" : "s");
    for (int i = 0; i < count; i++)
        printf("    %d. %-20s v%s\n", i + 1, steps[i].name, steps[i].version);

    if (dry_run) {
        printf("\n(vista previa -- no se instaló nada. Repite sin --dry-run para aplicar)\n");
        sqlite3_close(db); json_decref(idx); free(steps);
        return 0;
    }
    printf("\n");

    /* Fase 1: decide para cada paso si hace falta red (y arma su job), o
     * si ya está en su última versión (se resuelve aquí mismo, sin red).
     * El respaldo para rollback se hace AQUÍ, antes de que la descarga
     * paralela de la fase 2 pise la caché vieja que save_history_snapshot
     * necesita leer. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Walloc-size-larger-than=" /* falso positivo de gcc: count siempre es chico y no-negativo aca */
    InstallJob *jobs = calloc((size_t)count, sizeof(InstallJob));
#pragma GCC diagnostic pop
    int njobs = 0;
    for (int i = 0; i < count; i++) {
        const char *name = steps[i].name;
        const char *pkg_repo, *avail_version, *manifest_sha256;
        lookup_in_index(idx, name, &pkg_repo, &avail_version, &manifest_sha256);
        if (!pkg_repo) pkg_repo = g_repos[0];

        char installed_version[64] = {0};
        int already = 0;
        sqlite3_stmt *st;
        sqlite3_prepare_v2(db, "SELECT version FROM installed WHERE name = ?;", -1, &st, NULL);
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            already = 1;
            snprintf(installed_version, sizeof(installed_version), "%s", (const char *)sqlite3_column_text(st, 0));
        }
        sqlite3_finalize(st);

        int is_target = is_requested(pkg_names, npkgs, name);

        if (already && avail_version && vercmp(installed_version, avail_version) >= 0) {
            printf("[+] %s ya está en su última versión (v%s), se omite.\n", name, installed_version);
            if (is_target) {
                sqlite3_stmt *up;
                sqlite3_prepare_v2(db, "UPDATE installed SET explicit = 1 WHERE name = ?;", -1, &up, NULL);
                sqlite3_bind_text(up, 1, name, -1, SQLITE_STATIC);
                sqlite3_step(up); sqlite3_finalize(up);
            }
            continue;
        }

        if (already) save_history_snapshot(db, name);

        InstallJob *job = &jobs[njobs++];
        snprintf(job->name, sizeof(job->name), "%s", name);
        job->repo             = pkg_repo;
        job->manifest_sha256  = manifest_sha256;
        job->already           = already;
        job->explicit_flag     = is_target ? 1 : (already ? get_explicit_flag(db, name) : 0);
        snprintf(job->manifest_path, sizeof(job->manifest_path), "%s/%s.manifest.json", COCO_CACHE, name);
    }

    if (njobs == 0) {
        sqlite3_close(db); json_decref(idx); free(steps); free(jobs);
        printf("[+] Listo. %d paquete(s) procesados.\n", count);
        return 0;
    }

    /* Fase 2: descarga manifests+tarballs de TODO el lote EN PARALELO --
     * de los N paquetes pedidos y de sus dependencias combinadas, todo
     * junto, no paquete por paquete. */
    fetch_and_verify_many(jobs, njobs);

    /* Fase 3: instala en el orden que dio libsolv (steps[]) -- ahí sí
     * importa la secuencia (dependencias antes que quien las necesita,
     * por los hooks post-install que puedan asumir que ya están). */
    int failed = 0, done = 0;
    for (int i = 0; i < count; i++) {
        InstallJob *job = NULL;
        for (int j = 0; j < njobs; j++)
            if (!strcmp(jobs[j].name, steps[i].name)) { job = &jobs[j]; break; }
        if (!job) continue; /* se resolvió en fase 1 sin red */

        printf("[*] %s: %s\n", job->already ? "Actualizando" : "Instalando", job->name);
        if (!job->manifest) {
            fprintf(stderr, "    [!] Descarga/verificación falló, se omite.\n");
            failed++; printf("\n"); continue;
        }

        if (install_from_manifest(job->name, job->repo, job->pkg_path, job->manifest,
                                   job->already ? "Actualizado" : "Instalado",
                                   job->explicit_flag) != 0)
            failed++;
        else
            done++;
        printf("\n");
    }

    for (int j = 0; j < njobs; j++) if (jobs[j].manifest) json_decref(jobs[j].manifest);
    free(jobs);
    sqlite3_close(db);
    json_decref(idx);
    free(steps);

    if (failed) { fprintf(stderr, "[!] %d paso(s) fallaron.\n", failed); return -1; }
    printf("[+] Listo. %d paquete(s) procesados.\n", done + (count - njobs));
    return 0;
}

/* coco upgrade [pkg] [--dry-run] */
int cmd_upgrade(const char *pkg_name, int dry_run) {
    load_config();
    init_dirs();

    char *idxbuf = read_file(COCO_INDEX);
    if (!idxbuf) { fprintf(stderr, "[!] Sin índice local. Ejecuta 'coco sync' primero.\n"); return -1; }
    json_error_t e;
    json_t *idx = json_loads(idxbuf, 0, &e);
    free(idxbuf);
    if (!idx) { fprintf(stderr, "[!] Índice corrupto.\n"); return -1; }

    sqlite3 *db = open_db();
    if (!db) { json_decref(idx); return -1; }

    typedef struct { char name[128]; char from[64]; char to[64]; } Upg;
    Upg *list = NULL; int count = 0, cap = 0;

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
            if (count == cap) { cap = cap ? cap * 2 : 8; list = realloc(list, cap * sizeof(Upg)); }
            snprintf(list[count].name, sizeof(list[count].name), "%s", name);
            snprintf(list[count].from, sizeof(list[count].from), "%s", cur);
            snprintf(list[count].to,   sizeof(list[count].to),   "%s", avail);
            count++;
        }
    }
    sqlite3_finalize(st);

    if (count == 0) {
        printf(pkg_name ? "[+] '%s' ya está en su versión más reciente.\n" : "[+] Todo está actualizado.\n", pkg_name);
        sqlite3_close(db); json_decref(idx); free(list);
        return 0;
    }

    printf("[*] Actualizaciones disponibles (%d):\n", count);
    for (int i = 0; i < count; i++)
        printf("    %-20s v%s -> v%s\n", list[i].name, list[i].from, list[i].to);

    if (dry_run) {
        printf("\n(vista previa -- no se actualizó nada. Repite sin --dry-run para aplicar)\n");
        sqlite3_close(db); json_decref(idx); free(list);
        return 0;
    }
    printf("\n");

    /* Respaldo ANTES de la descarga paralela, que va a pisar la caché vieja
     * que save_history_snapshot necesita leer (igual que en cmd_install). */
    for (int i = 0; i < count; i++) save_history_snapshot(db, list[i].name);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Walloc-size-larger-than=" /* falso positivo de gcc: count siempre es chico y no-negativo aca */
    InstallJob *jobs = calloc((size_t)count, sizeof(InstallJob));
#pragma GCC diagnostic pop
    for (int i = 0; i < count; i++) {
        InstallJob *job = &jobs[i];
        snprintf(job->name, sizeof(job->name), "%s", list[i].name);

        job->repo = g_repos[0];
        job->manifest_sha256 = NULL;
        json_t *entry = json_object_get(idx, list[i].name);
        if (entry) {
            const char *r = json_string_value(json_object_get(entry, "_repo"));
            if (r) job->repo = r;
            job->manifest_sha256 = json_string_value(json_object_get(entry, "manifest_sha256"));
        }

        job->already       = 1;
        job->explicit_flag = get_explicit_flag(db, list[i].name); /* preservar el flag que ya tenía */
        snprintf(job->manifest_path, sizeof(job->manifest_path), "%s/%s.manifest.json", COCO_CACHE, list[i].name);
    }

    fetch_and_verify_many(jobs, count);

    int failed = 0;
    for (int i = 0; i < count; i++) {
        InstallJob *job = &jobs[i];
        printf("[*] Actualizando: %s\n", job->name);
        if (!job->manifest) {
            fprintf(stderr, "    [!] Descarga/verificación falló, se omite.\n");
            failed++; printf("\n"); continue;
        }
        if (install_from_manifest(job->name, job->repo, job->pkg_path, job->manifest,
                                   "Actualizado", job->explicit_flag) != 0)
            failed++;
        printf("\n");
    }

    printf("[+] Actualización completa: %d ok, %d con errores.\n", count - failed, failed);

    for (int i = 0; i < count; i++) if (jobs[i].manifest) json_decref(jobs[i].manifest);
    free(jobs);
    sqlite3_close(db);
    json_decref(idx);
    free(list);
    return failed ? -1 : 0;
}

/* Borra archivos + todas las filas de DB de un paquete YA validado como
 * eliminable -- el caller decide si hace falta revisar dependientes antes
 * (cmd_remove sí; cmd_autoremove no, porque un huérfano por definición ya
 * no lo requiere ningún paquete explícito). Retorna cuántos archivos borró. */
static int remove_installed_package(sqlite3 *db, const char *pkg_name) {
    sqlite3_stmt *files;
    sqlite3_prepare_v2(db, "SELECT path FROM files WHERE package = ?;", -1, &files, NULL);
    sqlite3_bind_text(files, 1, pkg_name, -1, SQLITE_STATIC);

    int removed = 0;
    while (sqlite3_step(files) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(files, 0);
        if (path && unlink(path) == 0) removed++;
    }
    sqlite3_finalize(files);

    static const char *tables[] = { "files", "depends", "conflicts", "provides", "config_files" };
    for (size_t t = 0; t < sizeof(tables) / sizeof(tables[0]); t++) {
        char sql[64];
        snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE package = ?;", tables[t]);
        sqlite3_stmt *del;
        sqlite3_prepare_v2(db, sql, -1, &del, NULL);
        sqlite3_bind_text(del, 1, pkg_name, -1, SQLITE_STATIC);
        sqlite3_step(del);
        sqlite3_finalize(del);
    }

    sqlite3_stmt *del;
    sqlite3_prepare_v2(db, "DELETE FROM installed WHERE name = ?;", -1, &del, NULL);
    sqlite3_bind_text(del, 1, pkg_name, -1, SQLITE_STATIC);
    sqlite3_step(del); sqlite3_finalize(del);

    char cache_glob[512];
    snprintf(cache_glob, sizeof(cache_glob), "rm -f '%s/%s'.* 2>/dev/null", COCO_CACHE, pkg_name);
    if (system(cache_glob) != 0) { /* limpieza best-effort */ }

    return removed;
}

/* coco remove <pkg> */
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

    /* dep_name en vez de dep: un paquete puede requerir "curl>=1.2.0" y aun
     * así tiene que bloquear el remove de "curl" a secas. También hay que
     * revisar `provides`: si alguien depende de una capacidad virtual
     * ("editor") que justo provee pkg_name, borrar pkg_name lo rompería
     * igual aunque nadie lo mencione por su nombre real. */
    sqlite3_stmt *rdep;
    sqlite3_prepare_v2(db,
        "SELECT DISTINCT package FROM depends "
        "WHERE dep_name = ?1 "
        "   OR dep_name IN (SELECT dep_name FROM provides WHERE package = ?1);",
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
    int removed = remove_installed_package(db, pkg_name);

    printf("[+] '%s' eliminado (%d archivos borrados). El historial para rollback se conserva.\n", pkg_name, removed);
    sqlite3_close(db);
    return 0;
}

/* coco autoremove [--dry-run] ─ Elimina paquetes que quedaron instalados
 * SOLO como dependencia (explicit=0 en la tabla `installed`) y que ya
 * ningún paquete explícito necesita, siguiendo la tabla `depends` desde
 * las raíces explícitas hasta un punto fijo (como 'apt autoremove'). */
int cmd_autoremove(int dry_run) {
    sqlite3 *db = open_db();
    if (!db) return -1;

    typedef struct { char name[128]; int keep; } Row;
    Row *rows = NULL; int n = 0, cap = 0;

    sqlite3_stmt *st;
    sqlite3_prepare_v2(db, "SELECT name, explicit FROM installed;", -1, &st, NULL);
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) { cap = cap ? cap * 2 : 16; rows = realloc(rows, (size_t)cap * sizeof(Row)); }
        snprintf(rows[n].name, sizeof(rows[n].name), "%s", (const char *)sqlite3_column_text(st, 0));
        rows[n].keep = sqlite3_column_int(st, 1) ? 1 : 0; /* las raíces explícitas ya nacen "keep" */
        n++;
    }
    sqlite3_finalize(st);

    /* Punto fijo: cada raíz "keep" propaga "keep" a lo que requiere (según
     * `depends`), resolviendo tanto por nombre real como por `provides`
     * (paquetes virtuales -- si "app1" depende de la capacidad "editor" y
     * "vim" es quien la provee, marcar "vim" como keep aunque nadie lo haya
     * pedido por su nombre). Repite hasta que una pasada no cambie nada. */
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < n; i++) {
            if (!rows[i].keep) continue;
            sqlite3_stmt *dst;
            sqlite3_prepare_v2(db, "SELECT DISTINCT dep_name FROM depends WHERE package = ?;", -1, &dst, NULL);
            sqlite3_bind_text(dst, 1, rows[i].name, -1, SQLITE_STATIC);
            while (sqlite3_step(dst) == SQLITE_ROW) {
                const char *depname = (const char *)sqlite3_column_text(dst, 0);
                if (!depname) continue;

                /* match directo por nombre */
                for (int k = 0; k < n; k++) {
                    if (!rows[k].keep && strcmp(rows[k].name, depname) == 0) {
                        rows[k].keep = 1;
                        changed = 1;
                    }
                }

                /* match vía provides (paquete virtual) */
                sqlite3_stmt *prov;
                sqlite3_prepare_v2(db, "SELECT package FROM provides WHERE dep_name = ?;", -1, &prov, NULL);
                sqlite3_bind_text(prov, 1, depname, -1, SQLITE_STATIC);
                while (sqlite3_step(prov) == SQLITE_ROW) {
                    const char *provider = (const char *)sqlite3_column_text(prov, 0);
                    if (!provider) continue;
                    for (int k = 0; k < n; k++) {
                        if (!rows[k].keep && strcmp(rows[k].name, provider) == 0) {
                            rows[k].keep = 1;
                            changed = 1;
                        }
                    }
                }
                sqlite3_finalize(prov);
            }
            sqlite3_finalize(dst);
        }
    }

    int orphan_count = 0;
    for (int i = 0; i < n; i++) if (!rows[i].keep) orphan_count++;

    if (orphan_count == 0) {
        printf("[+] No hay paquetes huérfanos que limpiar.\n");
        free(rows); sqlite3_close(db);
        return 0;
    }

    printf("[*] Paquetes instalados como dependencia y ya no requeridos (%d):\n", orphan_count);
    for (int i = 0; i < n; i++) if (!rows[i].keep) printf("    - %s\n", rows[i].name);

    if (dry_run) {
        printf("\n(vista previa -- no se eliminó nada. Repite sin --dry-run para aplicar)\n");
        free(rows); sqlite3_close(db);
        return 0;
    }
    printf("\n");

    int removed = 0;
    for (int i = 0; i < n; i++) {
        if (rows[i].keep) continue;
        printf("[*] Eliminando huérfano: %s\n", rows[i].name);
        remove_installed_package(db, rows[i].name);
        removed++;
    }

    printf("[+] Autoremove: %d paquete(s) eliminado(s).\n", removed);
    free(rows);
    sqlite3_close(db);
    return 0;
}

/* coco rollback <pkg> ─ Reinstala la última versión respaldada en el historial. */
int cmd_rollback(const char *pkg_name) {
    sqlite3 *db = open_db();
    if (!db) return -1;

    sqlite3_stmt *st;
    sqlite3_prepare_v2(db,
        "SELECT version, manifest_path, pkg_path FROM history WHERE package = ? ORDER BY id DESC LIMIT 1;",
        -1, &st, NULL);
    sqlite3_bind_text(st, 1, pkg_name, -1, SQLITE_STATIC);

    if (sqlite3_step(st) != SQLITE_ROW) {
        fprintf(stderr, "[!] No hay versiones anteriores respaldadas para '%s'.\n", pkg_name);
        sqlite3_finalize(st); sqlite3_close(db);
        return -1;
    }

    char version[64], manifest_path[560], pkg_path[560];
    snprintf(version,       sizeof(version),       "%s", (const char *)sqlite3_column_text(st, 0));
    snprintf(manifest_path, sizeof(manifest_path), "%s", (const char *)sqlite3_column_text(st, 1));
    snprintf(pkg_path,      sizeof(pkg_path),      "%s", (const char *)sqlite3_column_text(st, 2));
    sqlite3_finalize(st);

    /* Preserva el flag explicit que ya tenía (o 1 por defecto si el
     * paquete ya no está instalado -- volver de un rollback es una
     * acción directa del usuario). */
    int explicit_flag = get_explicit_flag(db, pkg_name);
    sqlite3_close(db);

    if (access(manifest_path, F_OK) != 0 || access(pkg_path, F_OK) != 0) {
        fprintf(stderr, "[!] El respaldo de '%s' v%s ya no está en disco (%s).\n", pkg_name, version, manifest_path);
        return -1;
    }

    printf("[*] Regresando '%s' a v%s (desde el historial local)...\n", pkg_name, version);

    char *mbuf = read_file(manifest_path);
    if (!mbuf) { fprintf(stderr, "[!] No se pudo leer el manifiesto respaldado.\n"); return -1; }
    json_error_t e;
    json_t *manifest = json_loads(mbuf, 0, &e);
    free(mbuf);
    if (!manifest) { fprintf(stderr, "[!] Manifiesto respaldado corrupto.\n"); return -1; }

    const char *sha256_exp = json_string_value(json_object_get(manifest, "sha256"));
    if (sha256_exp && strlen(sha256_exp) == 64) {
        char computed[65] = {0};
        if (sha256_file(pkg_path, computed) != 0 || strcmp(computed, sha256_exp) != 0) {
            fprintf(stderr, "[!] El respaldo de '%s' v%s está corrupto. Rollback cancelado.\n", pkg_name, version);
            json_decref(manifest);
            return -1;
        }
    }

    int rc = install_from_manifest(pkg_name, "(historial local)", pkg_path, manifest, "Rollback", explicit_flag);
    json_decref(manifest);
    return rc;
}

/* coco search <term> */
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

/* coco list */
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
    sqlite3_prepare_v2(db, "SELECT name, version, installed_at FROM installed ORDER BY name;", -1, &stmt, NULL);

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

/* coco info <pkg> */
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
        sqlite3_prepare_v2(db, "SELECT version, installed_at FROM installed WHERE name = ?;", -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, pkg_name, -1, SQLITE_STATIC);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *inst_ver = (const char *)sqlite3_column_text(stmt, 0);
            printf("Estado:      Instalado (v%s el %s)\n", inst_ver, (const char *)sqlite3_column_text(stmt, 1));
            if (ver && vercmp(inst_ver, ver) < 0)
                printf("Actualización disponible: v%s -> v%s  (coco upgrade %s)\n", inst_ver, ver, pkg_name);

            sqlite3_stmt *fc;
            sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM files WHERE package = ?;", -1, &fc, NULL);
            sqlite3_bind_text(fc, 1, pkg_name, -1, SQLITE_STATIC);
            if (sqlite3_step(fc) == SQLITE_ROW) printf("Archivos:    %d\n", sqlite3_column_int(fc, 0));
            sqlite3_finalize(fc);

            sqlite3_stmt *hc;
            sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM history WHERE package = ?;", -1, &hc, NULL);
            sqlite3_bind_text(hc, 1, pkg_name, -1, SQLITE_STATIC);
            if (sqlite3_step(hc) == SQLITE_ROW && sqlite3_column_int(hc, 0) > 0)
                printf("Historial:   %d versión(es) respaldada(s) (coco rollback %s)\n", sqlite3_column_int(hc, 0), pkg_name);
            sqlite3_finalize(hc);
        } else {
            printf("Estado:      No instalado\n");
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }

    json_decref(idx);
    return 0;
}

/* coco log <pkg> */
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
