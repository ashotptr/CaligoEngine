#define _GNU_SOURCE 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <jansson.h>

void url_decode(char *src, char *dest) {
    char *p = src;
    char code[3] = {0};
    unsigned long ascii = 0;
    char *end = NULL;

    while(*p) {
        if(*p == '%') {
            memcpy(code, ++p, 2);

            ascii = strtoul(code, &end, 16);
            *dest++ = (char)ascii;
            p += 2;
        } 
        else if(*p == '+') {
            *dest++ = ' ';
            p++;
        } 
        else {
            *dest++ = *p++;
        }
    }

    *dest = '\0';
}

void sanitize_input(char *str) {
    char *p = str;

    while (*p) {
        if (*p == '"' || *p == '\'' || *p == ';' || *p == '|' || *p == '`' || *p == '$') {
            *p = ' '; 
        }

        p++;
    }
}

int main() {
    char *len_str = getenv("CONTENT_LENGTH");
    long len = 0;

    if (len_str) {
        len = strtol(len_str, NULL, 10);
    }

    char buffer[4096] = {0};
    char decoded_query[4096] = {0};

    if (len > 0 && (size_t)len < sizeof(buffer)) {
        fread(buffer, 1, len, stdin);
    } 
    else {
        char *qs = getenv("QUERY_STRING");

        if (qs) {
            strncpy(buffer, qs, sizeof(buffer) - 1);
        }
    }

    char session_id[128] = "default_user";
    char query[4096] = "";

    json_error_t error;
    json_t *root = json_loads(buffer, 0, &error);

    if (root) {
        json_t *sid_obj = json_object_get(root, "session_id");

        if (json_is_string(sid_obj)) {
            strncpy(session_id, json_string_value(sid_obj), sizeof(session_id) - 1);
        }

        json_t *q_obj = json_object_get(root, "query");

        if (json_is_string(q_obj)) {
            strncpy(query, json_string_value(q_obj), sizeof(query) - 1);
        }

        json_decref(root);
    }

    sanitize_input(session_id);
    sanitize_input(query);

    char cwd[1024];

    if (!getcwd(cwd, sizeof(cwd))){
        return 1;
    } 

    char command[8192];
    
    snprintf(command, sizeof(command), 
        "export PATH=\"%s/venv/bin:$PATH\"; "
        "export HTTP_X_SESSION_ID=\"%s\"; "
        "\"%s/venv/bin/python3\" \"%s/cgi_bin/ai_engine.py\" --query \"%s\" 2>&1", 
        cwd, session_id, cwd, cwd, query);

    FILE *fp = popen(command, "r");

    if (fp == NULL) {
        printf("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n");

        printf("{\"answer\": \"Error: Failed to launch AI engine.\", \"logs\": \"\"}");

        return 1;
    }

    size_t capacity = 16384;
    size_t data_size = 0;
    char *response_body = malloc(capacity);
    
    if (!response_body) {
        pclose(fp);

        return 1;
    }

    response_body[0] = '\0';

    char line[1024];

    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t line_len = strlen(line);
        
        if (data_size + line_len + 1 > capacity) {
            capacity *= 2;
            char *new_buf = realloc(response_body, capacity);

            if (!new_buf) {
                free(response_body);

                pclose(fp);

                return 1;
            }

            response_body = new_buf;
        }
        
        strcat(response_body, line);

        data_size += line_len;
    }

    pclose(fp);

    printf("HTTP/1.1 200 OK\r\n");
    printf("Content-Type: application/json; charset=utf-8\r\n");
    printf("Content-Length: %zu\r\n", data_size);
    printf("Connection: close\r\n"); 
    printf("Cache-Control: no-cache, no-transform\r\n");
    printf("\r\n");

    if (data_size > 0) {
        printf("%s", response_body);
    } 
    else {
        printf("{\"answer\": \"Error: No response from AI.\", \"logs\": \"\"}");
    }
    
    fflush(stdout);

    free(response_body);
    
    return 0;
}