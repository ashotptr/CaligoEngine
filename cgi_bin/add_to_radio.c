#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <jansson.h>
#include <getopt.h>
#include <time.h>
#include <stdint.h>
#include <libgen.h>

#define TARGET_BITRATE "128k"
#define TARGET_SAMPLE_RATE "44100"
#define TARGET_CHANNELS "2"

void print_usage(const char* prog_name) {
    printf("Usage: %s -f <file> -o <output_dir> -a <artist> -l <album> -t <title>\n", prog_name);
    printf("  -f : Path to input audio file\n");
    printf("  -o : Path to output directory\n");
    printf("  -a : Artist Name\n");
    printf("  -l : Album Name\n");
    printf("  -t : Song Title\n");
}

void generate_filename_hash(char *dest, const char *artist, const char *album, const char *title, const char *ext) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    const char *inputs[] = {artist, album, title, NULL};
    
    for (int i = 0; inputs[i]; i++) {
        const char *p = inputs[i];

        while (*p) {
            hash ^= (unsigned char)*p++;
            hash *= 0x100000001b3ULL;
        }
        
        hash ^= '|';
        hash *= 0x100000001b3ULL;
    }

    sprintf(dest, "%lx%s", (unsigned long)hash, ext);
}

int is_file_valid(const char* path) {
    struct stat st;

    if (stat(path, &st) == 0) {
        return (st.st_size > 1024);
    }

    return 0;
}

int process_track(const char *input_path, const char *output_dir, const char *playlist_path, const char *artist, const char *album, const char *title) {
    char clean_name[256];

    generate_filename_hash(clean_name, artist, album, title, ".mp3");
    
    char output_path[512];

    snprintf(output_path, sizeof(output_path), "%s/%s", output_dir, clean_name);

    printf("[-] Processing: %s\n", title);

    char cmd[2048];

    snprintf(cmd, sizeof(cmd), 
        "ffmpeg -y -v error -i \"%s\" " 
        "-vn "
        "-acodec libmp3lame "
        "-ar %s "
        "-ac %s " 
        "-b:a %s "
        "-minrate %s "
        "-maxrate %s "
        "-bufsize %s "
        "-f mp3 \"%s\"",
        input_path, TARGET_SAMPLE_RATE, TARGET_CHANNELS, TARGET_BITRATE,
        TARGET_BITRATE, TARGET_BITRATE, TARGET_BITRATE, output_path);
        
    if (system(cmd) != 0 || !is_file_valid(output_path)) {
        fprintf(stderr, "[!] Error: Processing failed for %s\n", title);

        return 0;
    }

    json_error_t error;
    json_t *root = json_load_file(playlist_path, 0, &error);

    if (!root) {
        root = json_array();
    }

    if (!json_is_array(root)) { 
        json_decref(root);

        root = json_array();
    }

    size_t index;
    json_t *value;
    int exists = 0;

    json_array_foreach(root, index, value) {
        json_t *j_file = json_object_get(value, "file");

        if (json_is_string(j_file) && strcmp(json_string_value(j_file), clean_name) == 0) {
            exists = 1;
            
            break;
        }
    }

    if (!exists) {
        json_t *entry = json_object();

        json_object_set_new(entry, "file", json_string(clean_name));
        json_object_set_new(entry, "artist", json_string(artist));
        json_object_set_new(entry, "album", json_string(album));
        json_object_set_new(entry, "title", json_string(title));

        json_array_append_new(root, entry);

        json_dump_file(root, playlist_path, JSON_INDENT(4));

        printf("[+] Added to playlist: %s\n", title);
    }
    else {
        printf("[*] File updated (Entry exists): %s\n", title);
    }

    json_decref(root);

    return 1;
}

int main(int argc, char *argv[]) {
    int opt;
    char *input_file = NULL;
    char *json_batch = NULL;
    char *output_dir = NULL;
    char *artist = "Unknown Artist";
    char *album = "Unknown Album";
    char *title = "Unknown Title";

    while ((opt = getopt(argc, argv, "f:j:o:a:l:t:")) != -1) {
        switch (opt) {
            case 'f':
                input_file = optarg;
                break;
            case 'j':
                json_batch = optarg;
                break;
            case 'o':
                output_dir = optarg;
                break;
            case 'a':
                artist = optarg;
                break;
            case 'l':
                album = optarg;
                break;
            case 't':
                title = optarg;
                break;
            default:
                print_usage(argv[0]);

                return 1;
        }
    }

    if (!output_dir) {
        fprintf(stderr, "Error: Output directory (-o) is required.\n");

        return 1;
    }

    struct stat st = {0};

    if (stat(output_dir, &st) == -1) {
        mkdir("public_html", 0755);

        mkdir(output_dir, 0755);
    }

    char *playlist_path = malloc(strlen(output_dir) + 32);

    sprintf(playlist_path, "%s/playlist.json", output_dir);

    if (json_batch) {
        printf("[-] Batch Mode: %s\n", json_batch);

        json_error_t error;
        json_t *root = json_load_file(json_batch, 0, &error);
        
        if (!root) {
            fprintf(stderr, "Error loading batch JSON: %s\n", error.text);

            free(playlist_path); return 1;
        }

        const char *b_artist = json_string_value(json_object_get(root, "albumArtist"));
        const char *b_album  = json_string_value(json_object_get(root, "albumTitle"));

        json_t *tracks = json_object_get(root, "tracks");

        char *path_copy = malloc(strlen(json_batch) + 1);

        if (path_copy) {
            strcpy(path_copy, json_batch);
            
            char *json_dir = dirname(path_copy);

            size_t i;
            json_t *track;

            json_array_foreach(tracks, i, track) {
                const char *t_file = json_string_value(json_object_get(track, "file"));
                const char *t_title = json_string_value(json_object_get(track, "title"));
                
                if (t_file && t_title) {
                    char full_path[1024];

                    snprintf(full_path, sizeof(full_path), "%s/%s", json_dir, t_file);

                    process_track(full_path, output_dir, playlist_path, b_artist, b_album, t_title);
                }
            }

            free(path_copy);
        }

        json_decref(root);
    } 
    else if (input_file) {
        process_track(input_file, output_dir, playlist_path, artist, album, title);
    } 
    else {
        fprintf(stderr, "Error: Either -f (single) or -j (batch) is required.\n");

        print_usage(argv[0]);

        free(playlist_path);

        return 1;
    }

    free(playlist_path);

    return 0;
}