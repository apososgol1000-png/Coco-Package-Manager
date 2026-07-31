#ifndef COCO_GPGVERIFY_H
#define COCO_GPGVERIFY_H

/* Verifica una firma detached (.sig) de data_path contra un llavero de
 * confianza usando gpgv (el mismo verificador que usa apt-secure).
 *
 * Retorna:
 *   1  si la firma es válida
 *   0  si la firma es inválida o no coincide con el llavero
 *  -1  si no se pudo verificar (gpgv no encontrado, archivos faltantes, etc.)
 */
int gpg_verify_detached(const char *data_path, const char *sig_path, const char *keyring_path);

#endif
