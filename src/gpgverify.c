#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "gpgverify.h"

int gpg_verify_detached(const char *data_path, const char *sig_path, const char *keyring_path) {
    if (access(sig_path, F_OK) != 0) {
        fprintf(stderr, "  [!] No hay firma (%s)\n", sig_path);
        return -1;
    }
    if (access(keyring_path, F_OK) != 0) {
        fprintf(stderr, "  [!] Sin llavero de confianza (%s)\n", keyring_path);
        return -1;
    }

    char cmd[2048];
    /* gpgv: el mismo verificador minimalista que usa apt-secure/apt-key.
     * --keyring espera un llavero en formato "pubring.gpg" clásico
     * (secuencia de paquetes OpenPGP), no el .kbx nuevo. */
    snprintf(cmd, sizeof(cmd),
        "gpgv --keyring '%s' '%s' '%s' >/dev/null 2>&1",
        keyring_path, sig_path, data_path);

    int status = system(cmd);
    if (status == -1) return -1;
    if (!WIFEXITED(status)) return -1;

    return WEXITSTATUS(status) == 0 ? 1 : 0;
}
