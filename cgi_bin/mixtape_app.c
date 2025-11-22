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
    char *len_str = getenv("CONTENT_LENGTH");

    if (len_str) {
        int len = atoi(len_str);

        if (len > 0) {
            char *buffer = malloc(len + 1);

            if (buffer) {
                fread(buffer, 1, len, stdin);
                
                free(buffer);
            }
        }
    }

    json_t* root = json_object();
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
            char* album_folder_name = entry->d_name;

            char metadata_path[1024];
            snprintf(metadata_path, sizeof(metadata_path), "%s/%s/metadata.json", ROOT_PATH, album_folder_name);

            json_error_t error;
            json_t* metadata = json_load_file(metadata_path, 0, &error);

            if (!metadata) {
                continue;
            }

            const char* album_title = json_string_value(json_object_get(metadata, "albumTitle"));
            const char* album_artist = json_string_value(json_object_get(metadata, "albumArtist"));
            const char* cover_file = json_string_value(json_object_get(metadata, "coverFile"));
            json_t* tracks_metadata = json_object_get(metadata, "tracks");

            if (!album_title || !album_artist || !cover_file || !json_is_array(tracks_metadata)) {
                json_decref(metadata);
                continue;
            }

            json_t* album = json_object();
            
            json_object_set_new(album, "name", json_string(album_title)); 
            
            json_object_set_new(album, "artist", json_string(album_artist)); 

            char cover_url[1024];
            snprintf(cover_url, sizeof(cover_url), "%s/%s/%s", URL_BASE, album_folder_name, cover_file);
            json_object_set_new(album, "cover", json_string(cover_url));

            json_t* tracks_output_array = json_array();
            size_t i;
            json_t* track_metadata;

            json_array_foreach(tracks_metadata, i, track_metadata) {
                const char* track_file = json_string_value(json_object_get(track_metadata, "file"));
                const char* track_title = json_string_value(json_object_get(track_metadata, "title"));

                if (!track_file || !track_title) {
                    continue;
                }

                char audio_url[1024];
                snprintf(audio_url, sizeof(audio_url), "%s/%s/%s", URL_BASE, album_folder_name, track_file);

                json_t* track_output = json_object();

                json_object_set_new(track_output, "title", json_string(track_title));
                json_object_set_new(track_output, "audio_url", json_string(audio_url));
                json_array_append_new(tracks_output_array, track_output);
            }

            json_object_set_new(album, "tracks", tracks_output_array);
            json_array_append_new(albums, album);
            json_decref(metadata);
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