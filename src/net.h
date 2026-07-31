#ifndef COCO_NET_H
#define COCO_NET_H

/* Descarga `url` a `dest_path`.
 * Retorna 0 en éxito, -1 en error. */
int coco_download(const char *url, const char *dest_path);

/* Un archivo a descargar como parte de un lote paralelo. url/dest_path
 * deben seguir siendo válidos hasta que coco_download_many() retorne
 * (no se copian). */
typedef struct {
    const char *url;
    const char *dest_path;
} CocoDownloadJob;

/* Descarga varios archivos EN PARALELO usando la interfaz multi de libcurl
 * (varios sockets abiertos a la vez en vez de uno por uno). Pensado para
 * lotes tipo "los manifest.json de todo el plan de instalación" o "el
 * index.json de cada repo configurado".
 *
 * jobs/count = el lote a descargar.
 * ok_out     = si no es NULL, arreglo de `count` ints que se llena con
 *              1 (éxito) o 0 (falló ese archivo en particular) por índice.
 *              Los demás jobs del lote siguen su curso aunque uno falle.
 *
 * A diferencia de coco_download(), NO se muestra una barra de progreso por
 * archivo (con varios a la vez se vuelve ilegible); se imprime un resumen
 * antes/después desde quien llama.
 *
 * Retorna cuántos jobs tuvieron éxito (0..count). */
int coco_download_many(CocoDownloadJob *jobs, int count, int *ok_out);

#endif
