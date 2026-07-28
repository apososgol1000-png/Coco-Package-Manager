#ifndef COCO_PKG_H
#define COCO_PKG_H

/* Todos los comandos retornan 0 en éxito, distinto de 0 en error,
 * para que main() pueda propagar el exit code correcto. */

int cmd_sync   (void);
int cmd_install(const char *pkg_name);
int cmd_upgrade(const char *pkg_name); /* NULL = actualizar todos los paquetes instalados */
int cmd_remove (const char *pkg_name);
int cmd_search (const char *term);
int cmd_list   (void);
int cmd_info   (const char *pkg_name);
int cmd_log    (const char *pkg_name);

#endif
