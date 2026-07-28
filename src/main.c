#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "pkg.h"

static void usage(void) {
    printf("Uso: coco <comando> [paquete]\n\n");
    printf("Comandos:\n");
    printf("  sync              Actualizar índice de repositorios\n");
    printf("  install <pkg>     Instalar (o actualizar si ya existe) un paquete\n");
    printf("  upgrade [pkg]     Actualizar un paquete, o todos si se omite\n");
    printf("  remove  <pkg>     Eliminar un paquete instalado\n");
    printf("  search  <term>    Buscar paquetes en el índice\n");
    printf("  list              Listar paquetes instalados\n");
    printf("  info    <pkg>     Información detallada de un paquete\n");
    printf("  log     <pkg>     Ver log de instalación\n");
    printf("\nEjemplos:\n");
    printf("  coco sync\n");
    printf("  coco install neofetch\n");
    printf("  coco upgrade            # actualiza todo lo instalado\n");
    printf("  coco upgrade neofetch   # actualiza solo ese paquete\n");
}

int main(int argc, char **argv) {
    /* Coco extrae los paquetes con rutas relativas ("usr/bin/algo").
     * Sin esto, quedarían relativas al directorio donde se invocó coco
     * en vez de a la raíz real del sistema. */
    if (chdir("/") != 0) {
        perror("[!] No se pudo cambiar a la raíz del sistema (/)");
        return 1;
    }

    printf("Coco Package Manager v0.3.0 — CoconutWAY\n\n");

    if (argc < 2) { usage(); return 1; }

    const char *cmd = argv[1];
    const char *arg = (argc >= 3) ? argv[2] : NULL;

    int rc = 0;

    if      (!strcmp(cmd, "sync"))                   rc = cmd_sync();
    else if (!strcmp(cmd, "install") && arg)         rc = cmd_install(arg);
    else if (!strcmp(cmd, "upgrade"))                rc = cmd_upgrade(arg); /* arg puede ser NULL: actualiza todo */
    else if (!strcmp(cmd, "remove")  && arg)         rc = cmd_remove(arg);
    else if (!strcmp(cmd, "search")  && arg)         rc = cmd_search(arg);
    else if (!strcmp(cmd, "list"))                   rc = cmd_list();
    else if (!strcmp(cmd, "info")    && arg)         rc = cmd_info(arg);
    else if (!strcmp(cmd, "log")     && arg)         rc = cmd_log(arg);
    else { usage(); return 1; }

    return rc == 0 ? 0 : 1;
}
