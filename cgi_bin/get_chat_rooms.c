#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jansson.h>
#include <unistd.h>

#define ROOM_CONFIG_FILE "chat_rooms.txt"

int main(void) {
    json_t *root_array = json_array();

    FILE *f = fopen(ROOM_CONFIG_FILE, "r");
    
    if (f) {
        char line[128];

        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            
            if (strlen(line) > 0) {
                json_t *room_obj = json_object();

                json_object_set_new(room_obj, "name", json_string(line));
                
                json_object_set_new(room_obj, "count", json_integer(0)); 
                
                json_array_append_new(root_array, room_obj);
            }
        }
        
        fclose(f);
    }

    char *json_str = json_dumps(root_array, 0);
    size_t len = json_str ? strlen(json_str) : 0;

    printf("HTTP/1.1 200 OK\r\n");
    printf("Content-Type: application/json\r\n");
    printf("Content-Length: %zu\r\n", len);
    printf("Cache-Control: no-cache\r\n");
    printf("\r\n");
    
    if (json_str) {
        printf("%s", json_str);

        free(json_str);
    }

    json_decref(root_array);

    return 0;
}