#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <jansson.h>

#define DATA_FILE "data/users.json"

json_t* load_db() {
    json_error_t error;
    struct stat st = {0};

    if (stat("data", &st) == -1) {
        mkdir("data", 0700);
    }
    
    if (access(DATA_FILE, F_OK) != -1) {
        json_t *db = json_load_file(DATA_FILE, 0, &error);
        
        if (db) {
            return db;
        }
    }

    return json_object();
}

void save_db(json_t *root) { 
    json_dump_file(root, DATA_FILE, JSON_INDENT(2)); 
}

int main(void) {
    char *len_str = getenv("CONTENT_LENGTH");
    size_t len = len_str ? atoi(len_str) : 0;
    char *buffer = malloc(len + 1);
    
    if (!buffer) {
        return 1;
    }

    fread(buffer, 1, len, stdin);
    
    buffer[len] = '\0';

    json_error_t error;
    json_t *input = json_loads(buffer, 0, &error);

    free(buffer);
    
    json_t *response = json_object();

    if (!input) {
        json_object_set_new(response, "status", json_string("error"));
        json_object_set_new(response, "message", json_string("Invalid JSON"));
    }
    else {
        const char *action = json_string_value(json_object_get(input, "action"));
        const char *username = json_string_value(json_object_get(input, "username"));

        json_t *db = load_db();

        if (!action || !username) {
            json_object_set_new(response, "status", json_string("error"));
            json_object_set_new(response, "message", json_string("Missing fields"));
        }
        else {
            json_t *user_obj = json_object_get(db, username);
            
            if (strcmp(action, "get_playlists") == 0) {
                if (user_obj) {
                    json_object_set(response, "playlists", json_object_get(user_obj, "playlists"));
                    json_object_set_new(response, "status", json_string("success"));
                }
                else {
                    json_object_set_new(response, "playlists", json_object());
                    json_object_set_new(response, "status", json_string("success"));
                }
            }
            else if (strcmp(action, "create_playlist") == 0) {
                const char *pl_name = json_string_value(json_object_get(input, "playlist_name"));
                
                if(user_obj && pl_name && strlen(pl_name) > 0) {
                    json_t *pls = json_object_get(user_obj, "playlists");
                    
                    if(!json_object_get(pls, pl_name)) {
                        json_object_set_new(pls, pl_name, json_array());
                        
                        save_db(db);
                    }

                    json_object_set_new(response, "status", json_string("success"));
                }
                else {
                    json_object_set_new(response, "status", json_string("error"));
                    json_object_set_new(response, "message", json_string("Invalid name"));
                }
            }
            else if (strcmp(action, "add_to_playlist") == 0) {
                const char *pl_name = json_string_value(json_object_get(input, "playlist_name"));
                json_t *track = json_object_get(input, "track");
                
                if(user_obj && pl_name && track) {
                    json_t *pls = json_object_get(user_obj, "playlists");
                    json_t *list = json_object_get(pls, pl_name);

                    if(list) {
                        json_array_append(list, track);

                        save_db(db);
                        
                        json_object_set_new(response, "status", json_string("success"));
                    }
                    else {
                        json_object_set_new(response, "status", json_string("error"));
                        json_object_set_new(response, "message", json_string("Playlist not found"));
                    }
                }
                else {
                    json_object_set_new(response, "status", json_string("error"));
                    json_object_set_new(response, "message", json_string("Missing track data"));
                }
            }
        }

        json_decref(db);
    }

    char *out = json_dumps(response, 0);
    size_t out_len = out ? strlen(out) : 0;

    printf("HTTP/1.1 200 OK\r\n");
    printf("Content-Type: application/json\r\n");
    printf("Content-Length: %zu\r\n", out_len);
    printf("Connection: close\r\n\r\n");
    
    if (out) {
        printf("%s", out);

        free(out);
    }

    json_decref(input);
    json_decref(response);
    
    return 0;
}