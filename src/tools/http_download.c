/*
 * http_download.c
 * vim: expandtab:ts=4:sts=4:sw=4
 *
 * Copyright (C) 2012 - 2019 James Booth <boothj5@gmail.com>
 * Copyright (C) 2020 William Wennerström <william@wstrm.dev>
 *
 * This file is part of Profanity.
 *
 * Profanity is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Profanity is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Profanity.  If not, see <https://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link the code of portions of this program with the OpenSSL library under
 * certain conditions as described in each individual source file, and
 * distribute linked combinations including the two.
 *
 * You must obey the GNU General Public License in all respects for all of the
 * code used other than OpenSSL. If you modify file(s) with this exception, you
 * may extend this exception to your version of the file(s), but you are not
 * obligated to do so. If you do not wish to do so, delete this exception
 * statement from your version. If you delete this exception statement from all
 * source files in the program, then also delete it here.
 *
 */

#define _GNU_SOURCE 1

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <curl/curl.h>
#include <gio/gio.h>
#include <pthread.h>
#include <assert.h>

#include "profanity.h"
#include "event/client_events.h"
#include "tools/http_download.h"
#include "config/preferences.h"
#include "ui/ui.h"
#include "ui/window.h"
#include "common.h"

#define FALLBACK_MSG ""

GSList *download_processes = NULL;

static int
_xferinfo(void *userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    HTTPDownload *download = (HTTPDownload *)userdata;

    pthread_mutex_lock(&lock);

    if (download->cancel) {
        pthread_mutex_unlock(&lock);
        return 1;
    }

    if (download->bytes_received == dlnow) {
        pthread_mutex_unlock(&lock);
        return 0;
    } else {
        download->bytes_received = dlnow;
    }

    unsigned int dlperc = 0;
    if (dltotal != 0) {
        dlperc = (100 * dlnow) / dltotal;
    }

    char *msg;
    if (asprintf(&msg, "Downloading '%s': %d%%", download->url, dlperc) == -1) {
        msg = strdup(FALLBACK_MSG);
    }
    win_update_entry_message(download->window, download->url, msg);
    free(msg);

    pthread_mutex_unlock(&lock);

    return 0;
}

#if LIBCURL_VERSION_NUM < 0x072000
static int
_older_progress(void *p, double dltotal, double dlnow, double ultotal, double ulnow)
{
    return _xferinfo(p, (curl_off_t)dltotal, (curl_off_t)dlnow, (curl_off_t)ultotal, (curl_off_t)ulnow);
}
#endif

char *http_filename_from_header(char *header) {
    const char *header_tag_cd = "Content-Disposition:";
    const int header_tag_cd_len = strlen(header_tag_cd);

    if (!header) {
        return NULL; // Bad header.
    }

    if (strncasecmp(header, header_tag_cd, header_tag_cd_len) == 0) {
        header += header_tag_cd_len; // Move to header content.
    } else {
        return NULL; // Not a CD header.
    }

    const char *filename_key = "filename=";
    const size_t filename_key_len = strlen(filename_key);

    char *value = strcasestr(header, filename_key);
    if (!value) {
        return NULL; // No filename key found.
    }

    value += filename_key_len; // Move to key value.

    char fn[4096];
    char *pf = fn;
    while(*value != '\0' && *value != ';') {
        *pf++ = *value++;
    }
    *pf = '\0';

    if (!strlen(fn)) {
        return NULL; // Empty tag.
    }

    return strdup(fn);
}

char *http_filename_from_url(const char *url) {
    const char *default_name = "index.html";

    GFile *file = g_file_new_for_uri(url);
    char *filename = g_file_get_basename(file);
    g_object_unref(file);

    if (g_strcmp0(filename, ".") == 0
            || g_strcmp0(filename, G_DIR_SEPARATOR_S) == 0) {
        g_free(filename);
        return strdup(default_name);
    }

    return filename;
}

static size_t _header_callback(char *data, size_t size, size_t nitems, void *userdata) {
    char *header = (char*)data;

    HTTPDownload *download = (HTTPDownload *)userdata;
    size *= nitems;

    if (download->filename != NULL) {
        return size; // No-op.
    }

    download->filename = http_filename_from_header(header);

    return size;
}

FILE *_get_filehandle(const char *directory, const char *filename) {
    gchar *fp;
    FILE *fh;

    // Explicitly use "." as directory if no directory has been passed.
    if (directory == NULL) {
        fp = g_build_filename(".", filename, NULL);
    } else {
        fp = g_build_filename(directory, filename, NULL);
    }

    fh = fopen(fp, "wb");
    g_free(fp);
    return fh;
}

static size_t _write_callback(void *buffer, size_t size, size_t nmemb, void *userdata) {
    HTTPDownload *download = (HTTPDownload *)userdata;
    size *= nmemb;

    if (download->filename == NULL) {
        download->filename = http_filename_from_url(download->url);
    }

    if (download->filename == NULL || download->directory == NULL) {
        return 0; // Missing file name or directory, write no data.
    }

    if (download->filehandle == NULL ) {
        FILE *fh = _get_filehandle(download->directory, download->filename);
        if (!fh) {
            return 0; // Unable to open file handle.
        }
        download->filehandle = fh;
    }

    return fwrite(buffer, size, nmemb, userdata);
}

void *
http_file_get(void *userdata)
{
    HTTPDownload *download = (HTTPDownload *)userdata;

    char *err = NULL;

    CURL *curl;
    CURLcode res;

    download->cancel = 0;
    download->bytes_received = 0;

    pthread_mutex_lock(&lock);
    char* msg;
    if (asprintf(&msg, "Downloading '%s': 0%%", download->url) == -1) {
        msg = strdup(FALLBACK_MSG);
    }
    win_print_http_transfer(download->window, msg, download->url);
    free(msg);

    char *cert_path = prefs_get_string(PREF_TLS_CERTPATH);
    pthread_mutex_unlock(&lock);

    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();

    curl_easy_setopt(curl, CURLOPT_URL, download->url);

    #if LIBCURL_VERSION_NUM >= 0x072000
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, _xferinfo);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, download);
    #else
    curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, _older_progress);
    curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, download);
    #endif
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, _header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void *)download);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)download);

    curl_easy_setopt(curl, CURLOPT_USERAGENT, "profanity");

    if (cert_path) {
        curl_easy_setopt(curl, CURLOPT_CAPATH, cert_path);
    }

    if ((res = curl_easy_perform(curl)) != CURLE_OK) {
        err = strdup(curl_easy_strerror(res));
    }

    curl_easy_cleanup(curl);
    curl_global_cleanup();

    if (download->filehandle) {
        fclose(download->filehandle);
    }

    pthread_mutex_lock(&lock);
    g_free(cert_path);

    if (err) {
        char *msg;
        if (download->cancel) {
            if (asprintf(&msg, "Downloading '%s' failed: Download was canceled", download->url) == -1) {
                msg = strdup(FALLBACK_MSG);
            }
        } else {
            if (asprintf(&msg, "Downloading '%s' failed: %s", download->url, err) == -1) {
                msg = strdup(FALLBACK_MSG);
            }
            win_update_entry_message(download->window, download->url, msg);
        }
        cons_show_error(msg);
        free(msg);
        free(err);
    } else {
        if (!download->cancel) {
            if (asprintf(&msg, "Downloading '%s': 100%%", download->url) == -1) {
                msg = strdup(FALLBACK_MSG);
            }
            win_update_entry_message(download->window, download->url, msg);
            win_mark_received(download->window, download->url);
            free(msg);
        }
    }

    download_processes = g_slist_remove(download_processes, download);
    pthread_mutex_unlock(&lock);

    free(download->url);
    free(download->filename);
    free(download->directory);
    free(download);

    return NULL;
}

void
http_download_cancel_processes(ProfWin *window)
{
    GSList *download_process = download_processes;
    while (download_process) {
        HTTPDownload *download = download_process->data;
        if (download->window == window) {
            download->cancel = 1;
            break;
        }
        download_process = g_slist_next(download_process);
    }
}

void
http_download_add_download(HTTPDownload *download)
{
    download_processes = g_slist_append(download_processes, download);
}
