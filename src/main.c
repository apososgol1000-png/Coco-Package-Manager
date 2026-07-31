#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "pkg.h"

static void usage(void) {
    printf("Uso: coco <comando> [paquete] [opciones]\n\n");
    printf("Comandos:\n");
    printf("  sync                    Actualizar índice de repositorios\n");
    printf("  install <pkg...> [--dry-run]  Instalar uno o varios (resuelve deps con libsolv)\n");
    printf("  upgrade [pkg] [--dry-run]  Actualizar un paquete, o todos si se omite\n");
    printf("  remove  <pkg>           Eliminar un paquete instalado\n");
    printf("  autoremove [--dry-run]  Eliminar huérfanos (instalados solo como dependencia)\n");
    printf("  rollback <pkg>          Regresar a la versión anterior respaldada\n");
    printf("  search  <term>          Buscar paquetes en el índice\n");
    printf("  list                    Listar paquetes instalados\n");
    printf("  info    <pkg>           Información detallada de un paquete\n");
    printf("  log     <pkg>           Ver log de instalación\n");
    printf("\nEjemplos:\n");
    printf("  coco sync\n");
    printf("  coco install neofetch --dry-run   # vista previa, no instala nada\n");
    printf("  coco install neofetch             # instala de verdad, con sus deps\n");
    printf("  coco install cpufetch fastfetch coconut-core  # varios a la vez, en paralelo\n");
    printf("  coco upgrade                      # actualiza todo lo instalado\n");
    printf("  coco rollback neofetch            # regresa a la version anterior\n");
    printf("  coco autoremove --dry-run         # ver qué se borraría, sin borrar nada\n");
}

int main(int argc, char **argv) {
    /* Coco extrae los paquetes con rutas relativas ("usr/bin/algo").
     * Sin esto, quedarían relativas al directorio donde se invocó coco
     * en vez de a la raíz real del sistema. */
    if (chdir("/") != 0) {
        perror("[!] No se pudo cambiar a la raíz del sistema (/)");
        return 1;
    }

    printf("Coco Package Manager v0.5.0 — CoconutWAY\n\n");

    if (argc < 2) { usage(); return 1; }

    const char *cmd = argv[1];

    /* Separar --dry-run del resto de argumentos, sin importar en qué
     * posición venga (coco install pkg --dry-run  o  coco install --dry-run pkg).
     * Los demás argumentos se juntan en `pkgs[]` -- 'install' puede recibir
     * varios a la vez ('coco install a b c'); el resto de comandos solo usa
     * `arg` (el primero), igual que antes. */
    int dry_run = 0;
    const char *pkgs[64];
    int npkgs = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--dry-run")) dry_run = 1;
        else if (npkgs < 64) pkgs[npkgs++] = argv[i];
    }
    const char *arg = npkgs > 0 ? pkgs[0] : NULL;

    int rc = 0;

    if      (!strcmp(cmd, "sync"))                    rc = cmd_sync();
    else if (!strcmp(cmd, "install")  && npkgs > 0)    rc = cmd_install(pkgs, npkgs, dry_run);
    else if (!strcmp(cmd, "upgrade"))                  rc = cmd_upgrade(arg, dry_run); /* arg NULL = todos */
    else if (!strcmp(cmd, "remove")   && arg)          rc = cmd_remove(arg);
    else if (!strcmp(cmd, "autoremove"))                rc = cmd_autoremove(dry_run);
    else if (!strcmp(cmd, "rollback") && arg)          rc = cmd_rollback(arg);
    else if (!strcmp(cmd, "search")   && arg)          rc = cmd_search(arg);
    else if (!strcmp(cmd, "list"))                     rc = cmd_list();
    else if (!strcmp(cmd, "info")     && arg)          rc = cmd_info(arg);
    else if (!strcmp(cmd, "log")      && arg)          rc = cmd_log(arg);
    else { usage(); return 1; }

    return rc == 0 ? 0 : 1;
}
