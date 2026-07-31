#include <stdio.h>
#include <stdlib.h>
#include <curl/curl.h>
#include "net.h"

static size_t write_file_cb(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

int coco_download(const char *url, const char *dest_path) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    FILE *fp = fopen(dest_path, "wb");
    if (!fp) {
        curl_easy_cleanup(curl);
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,    0L);  /* muestra barra de progreso */

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    fclose(fp);

    return (res == CURLE_OK) ? 0 : -1;
}

int coco_download_many(CocoDownloadJob *jobs, int count, int *ok_out) {
    if (ok_out) for (int i = 0; i < count; i++) ok_out[i] = 0;
    if (count <= 0) return 0;

    CURLM *multi = curl_multi_init();
    if (!multi) return 0;

    CURL **easies = calloc((size_t)count, sizeof(CURL *));
    FILE **files  = calloc((size_t)count, sizeof(FILE *));
    if (!easies || !files) {
        free(easies); free(files);
        curl_multi_cleanup(multi);
        return 0;
    }

    for (int i = 0; i < count; i++) {
        files[i] = fopen(jobs[i].dest_path, "wb");
        if (!files[i]) {
            fprintf(stderr, "  [!] No se pudo abrir '%s' para escritura\n", jobs[i].dest_path);
            continue;
        }
        CURL *eh = curl_easy_init();
        if (!eh) { fclose(files[i]); files[i] = NULL; continue; }

        curl_easy_setopt(eh, CURLOPT_URL,            jobs[i].url);
        curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION,  write_file_cb);
        curl_easy_setopt(eh, CURLOPT_WRITEDATA,      files[i]);
        curl_easy_setopt(eh, CURLOPT_FOLLOWLOCATION,  1L);
        curl_easy_setopt(eh, CURLOPT_NOPROGRESS,      1L); /* varias barras a la vez serían ilegibles */
        curl_easy_setopt(eh, CURLOPT_PRIVATE, (void *)(long)i); /* para identificar el job al leer resultados */

        easies[i] = eh;
        curl_multi_add_handle(multi, eh);
    }

    int still_running = 0;
    curl_multi_perform(multi, &still_running);
    while (still_running) {
        int numfds = 0;
        /* curl_multi_wait duerme hasta que haya actividad en algún socket
         * (o 1s de tope), en vez de hacer busy-loop con perform() a lo tonto. */
        curl_multi_wait(multi, NULL, 0, 1000, &numfds);
        curl_multi_perform(multi, &still_running);
    }

    int ok_count = 0;
    int msgs_left;
    CURLMsg *msg;
    while ((msg = curl_multi_info_read(multi, &msgs_left)) != NULL) {
        if (msg->msg != CURLMSG_DONE) continue;
        void *priv = NULL;
        curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &priv);
        int idx = (int)(long)priv;
        int success = (msg->data.result == CURLE_OK);
        if (!success)
            fprintf(stderr, "  [!] Falló '%s': %s\n", jobs[idx].url, curl_easy_strerror(msg->data.result));
        if (ok_out) ok_out[idx] = success;
        if (success) ok_count++;
    }

    for (int i = 0; i < count; i++) {
        if (easies[i]) { curl_multi_remove_handle(multi, easies[i]); curl_easy_cleanup(easies[i]); }
        if (files[i]) fclose(files[i]);
    }
    free(easies);
    free(files);
    curl_multi_cleanup(multi);

    return ok_count;
}
