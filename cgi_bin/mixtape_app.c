#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <jansson.h>
#include <dirent.h>
#include <sys/stat.h>

#define ROOT_PATH "public_html/mixtape"
#define URL_BASE "/mixtape"

int is_image(const char* name) {
    const char* ext = strrchr(name, '.');

    if (!ext) {
        return 0;
    }

    return strcasecmp(ext, ".jpg") == 0 || 
           strcasecmp(ext, ".jpeg") == 0 || 
           strcasecmp(ext, ".png") == 0;
}

int is_mp3(const char* name) {
    const char* ext = strrchr(name, '.');

    if (!ext) {
        return 0;
    }

    return strcasecmp(ext, ".mp3") == 0;
}

int main(void) {
    json_t* root = json_object();

    json_object_set_new(root, "artist", json_string("Her Personal Mixtape"));

    json_t* albums = json_array();

    DIR* dir = opendir(ROOT_PATH);

    if (dir == NULL) {
        json_object_set_new(root, "albums", albums);

        char* json_dump = json_dumps(root, 0);

        printf("HTTP/1.1 200 OK\r\n");
        printf("Content-Type: application/json\r\n");
        printf("Content-Length: %ld\r\n\r\n", strlen(json_dump));
        printf("%s", json_dump);

        json_decref(root);
        free(json_dump);

        return 0;
    }

    struct dirent* entry;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char album_path[1024];

        snprintf(album_path, sizeof(album_path), "%s/%s", ROOT_PATH, entry->d_name);

        struct stat path_stat;
        stat(album_path, &path_stat);

        if (S_ISDIR(path_stat.st_mode)) {
            char* album_name = entry->d_name;
            
            char cover_url[1024] = "";

            DIR* album_dir = opendir(album_path);

            if (album_dir == NULL) {
                continue;
            }

            struct dirent* file_entry;

            while ((file_entry = readdir(album_dir)) != NULL) {
                if (cover_url[0] == '\0' && is_image(file_entry->d_name)) {
                    snprintf(cover_url, sizeof(cover_url), "%s/%s/%s", URL_BASE, album_name, file_entry->d_name);
                }

                if (is_mp3(file_entry->d_name)) {
                    char audio_url[1024];

                    snprintf(audio_url, sizeof(audio_url), "%s/%s/%s", URL_BASE, album_name, file_entry->d_name);

                    json_t* track = json_object();

                    json_object_set_new(track, "title", json_string(file_entry->d_name));
                    json_object_set_new(track, "cover", json_string(cover_url));
                    json_object_set_new(track, "audio_url", json_string(audio_url));

                    json_array_append_new(albums, track);
                }
            }

            closedir(album_dir);
        }
    }

    closedir(dir);

    json_object_set_new(root, "albums", albums);

    char* json_dump = json_dumps(root, JSON_INDENT(2));

    printf("HTTP/1.1 200 OK\r\n");
    printf("Content-Type: application/json\r\n");
    printf("Content-Length: %ld\r\n\r\n", strlen(json_dump));
    printf("%s", json_dump);

    json_decref(root);
    free(json_dump);

    return 0;
}