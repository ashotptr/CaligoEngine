#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <jansson.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define USERS_FILE "data/users.json"
#define PASSWORD_CHARS "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*()"

void generate_password(char* password, size_t length) {
    srand(time(NULL));

    for (size_t i = 0; i < length; i++) {
        password[i] = PASSWORD_CHARS[rand() % strlen(PASSWORD_CHARS)];
    }

    password[length] = '\0';
}

json_t* load_users() {
    FILE* file = fopen(USERS_FILE, "r");

    if (!file) {
        return json_object();
    }

    json_error_t error;
    json_t* users = json_loadf(file, 0, &error);

    fclose(file);

    if (!users) {
        return json_object();
    }

    return users;
}

void save_users(json_t* users) {
    struct stat st = {0};

    if (stat("data", &st) == -1) {
        mkdir("data", 0700);
    }

    FILE* file = fopen(USERS_FILE, "w");

    if (!file) {
        return;
    }

    json_dumpf(users, file, JSON_INDENT(2));

    fclose(file);
}

void handle_register(json_t* request_data) {
    const char* name = json_string_value(json_object_get(request_data, "name"));
    const char* password = json_string_value(json_object_get(request_data, "password"));
    int generate = json_is_true(json_object_get(request_data, "generate"));

    if (!name || strlen(name) == 0) {
        const char *msg = "{\"success\": false, \"error\": \"Name is required\"}\n";
        
        printf("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %ld\r\n\r\n%s", strlen(msg), msg);
        
        return;
    }

    json_t* users = load_users();

    if (json_object_get(users, name)) {
        const char *msg = "{\"success\": false, \"error\": \"User already exists\"}\n";
        
        printf("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %ld\r\n\r\n%s", strlen(msg), msg);
        
        json_decref(users);
        
        return;
    }

    char final_password[33];

    if (generate || !password || strlen(password) == 0) {
        generate_password(final_password, 12);
    }
    else {
        strncpy(final_password, password, sizeof(final_password) - 1);

        final_password[sizeof(final_password) - 1] = '\0';
    }
    
    json_t *user_obj = json_object();

    json_object_set_new(user_obj, "password", json_string(final_password));
    json_object_set_new(user_obj, "playlists", json_object());
    json_object_set_new(users, name, user_obj);

    save_users(users);

    char body[512];
    
    snprintf(body, sizeof(body), "{\"success\": true, \"name\": \"%s\", \"password\": \"%s\"}\n", name, final_password);
    
    printf("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %ld\r\n\r\n%s", strlen(body), body);

    json_decref(users);
}

void handle_login(json_t* request_data) {
    const char* name = json_string_value(json_object_get(request_data, "name"));
    const char* password = json_string_value(json_object_get(request_data, "password"));

    if (!name || !password) {
        const char *msg = "{\"success\": false, \"error\": \"Name and password are required\"}\n";
        printf("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %ld\r\n\r\n%s", strlen(msg), msg);
        return;
    }

    json_t* users = load_users();
    json_t* user_obj = json_object_get(users, name);

    if (!user_obj) {
        const char *msg = "{\"success\": false, \"error\": \"User not found\"}\n";
        
        printf("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %ld\r\n\r\n%s", strlen(msg), msg);
        
        json_decref(users);
        
        return;
    }

    const char* stored_pwd_str = json_string_value(json_object_get(user_obj, "password"));
    
    if (!stored_pwd_str && json_is_string(user_obj)) {
        stored_pwd_str = json_string_value(user_obj);
    }

    if (stored_pwd_str && strcmp(password, stored_pwd_str) == 0) {
        char body[512];

        snprintf(body, sizeof(body), "{\"success\": true, \"name\": \"%s\"}\n", name);
        
        printf("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %ld\r\n\r\n%s", strlen(body), body);
    }
    else {
        const char *msg = "{\"success\": false, \"error\": \"Invalid password\"}\n";
        
        printf("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %ld\r\n\r\n%s", strlen(msg), msg);
    }

    json_decref(users);
}

int main() {
    const char* method = getenv("REQUEST_METHOD");
    const char* content_length_str = getenv("CONTENT_LENGTH");
    
    FILE *log_file = fopen("cgi_debug.log", "a"); 

    if (log_file) {
        fprintf(log_file, "--- New Request ---\n");
        fprintf(log_file, "Method: %s\n", method ? method : "NULL");
        fprintf(log_file, "Content-Length Env: %s\n", content_length_str ? content_length_str : "NULL");
    }

    if (!method || strcmp(method, "POST") != 0) {
        const char *msg = "{\"success\": false, \"error\": \"Only POST method is supported\"}\n";
        
        printf("HTTP/1.1 405 Method Not Allowed\r\nContent-Type: application/json\r\nContent-Length: %ld\r\n\r\n%s", strlen(msg), msg);
        
        if (log_file){
            fclose(log_file);
        }
        
        return 1;
    }

    int content_length = content_length_str ? atoi(content_length_str) : 0;

    if (content_length <= 0 || content_length > 10000) {
        const char *msg = "{\"success\": false, \"error\": \"Invalid content length\"}\n";
        
        printf("HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: %ld\r\n\r\n%s", strlen(msg), msg);
        
        if (log_file) {
            fclose(log_file);
        }
        
        return 1;
    }

    char* body = (char*)malloc(content_length + 1);

    if (!body) {
        const char *msg = "{\"success\": false, \"error\": \"Memory allocation failed\"}\n";
        
        printf("HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: %ld\r\n\r\n%s", strlen(msg), msg);
        
        if (log_file) {
            fclose(log_file);
        }
        
        return 1;
    }

    size_t bytes_read = fread(body, 1, content_length, stdin);

    body[bytes_read] = '\0';

    if (log_file) {
        fprintf(log_file, "Expected Length: %d\n", content_length);
        fprintf(log_file, "Actual Bytes Read: %zu\n", bytes_read);
        fprintf(log_file, "Raw Body Content: [%s]\n", body);
        fprintf(log_file, "Hex Dump: ");

        for(size_t i = 0; i < bytes_read; i++) {
            fprintf(log_file, "%02X ", (unsigned char)body[i]);
        }

        fprintf(log_file, "\n");
    }

    json_error_t error;
    json_t* request_data = json_loads(body, 0, &error);

    free(body);

    if (!request_data) {
        if (log_file) {
            fprintf(log_file, "JSON Error: %s at line %d column %d\n", error.text, error.line, error.column);
            fclose(log_file);
        }
        
        char msg[256];
        
        snprintf(msg, sizeof(msg), "{\"success\": false, \"error\": \"Invalid JSON\", \"debug_msg\": \"%s\"}\n", error.text);
        
        printf("HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: %ld\r\n\r\n%s", strlen(msg), msg);
        
        return 1;
    }
    
    if (log_file) {
        fprintf(log_file, "JSON Parsed Successfully\n");

        fclose(log_file);
    }

    const char* action = json_string_value(json_object_get(request_data, "action"));

    if (!action) {
        const char *msg = "{\"success\": false, \"error\": \"Action is required\"}\n";

        printf("HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: %ld\r\n\r\n%s", strlen(msg), msg);
        
        json_decref(request_data);
        
        return 1;
    }

    if (strcmp(action, "register") == 0) {
        handle_register(request_data);
    }
    else if (strcmp(action, "login") == 0) {
        handle_login(request_data);
    }
    else {
        const char *msg = "{\"success\": false, \"error\": \"Unknown action\"}\n";

        printf("HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: %ld\r\n\r\n%s", strlen(msg), msg);
    }

    json_decref(request_data);
    
    return 0;
}