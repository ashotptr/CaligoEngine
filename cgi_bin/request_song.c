#define _GNU_SOURCE 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

int main() {
    char *len_str = getenv("CONTENT_LENGTH");
    long len = 0;
    char buffer[2048];
    char query[2048];
    char decoded_query[2048];

    printf("HTTP/1.1 200 OK\r\n");
    printf("Content-Type: text/plain; charset=utf-8\r\n");
    printf("Cache-Control: no-cache, no-transform\r\n");
    printf("X-Accel-Buffering: no\r\n");
    printf("Transfer-Encoding: chunked\r\n");
    printf("\r\n");

    fflush(stdout);

    if (len_str) {
        len = strtol(len_str, NULL, 10);

        if (len > 2047) {
            len = 2047;
        }

        fread(buffer, 1, len, stdin);

        buffer[len] = '\0';

        char *start = strstr(buffer, "query=");

        if (start) {
            strcpy(query, start + 6);

            url_decode(query, decoded_query);
            
            for(int i=0; decoded_query[i]; i++) {
                if (strchr("; |&`$\"\\'<>", decoded_query[i])) {
                    decoded_query[i] = '_';
                }
            }

            char cwd[1024];

            if (!getcwd(cwd, sizeof(cwd))) {
                return 1;
            }

            char command[4096];

            snprintf(command, sizeof(command), "export PATH=\"%s/venv/bin:$PATH\"; \"%s/venv/bin/python3\" \"%s/cgi_bin/downloader.py\" \"%s\" 2>&1", cwd, cwd, cwd, decoded_query);

            FILE *fp = popen(command, "r");

            if (fp == NULL) {
                char *err = "ERROR: Failed to launch engine.\n";

                printf("%lX\r\n%s\r\n", strlen(err), err);
                printf("0\r\n\r\n");

                fflush(stdout);

                return 1;
            }

            char line[1024];

            while (fgets(line, sizeof(line), fp) != NULL) {
                size_t line_len = strlen(line);

                if (line_len == 0) {
                    continue;
                }

                printf("%lX\r\n%s\r\n", line_len, line);

                fflush(stdout);
            }

            pclose(fp);

            printf("0\r\n\r\n");

            fflush(stdout);
            
        }
        else {
            char *msg = "ERROR: No query provided\n";

            printf("%lX\r\n%s\r\n", strlen(msg), msg);
            printf("0\r\n\r\n");
        }
    }
    else {
        char *msg = "ERROR: No content\n";

        printf("%lX\r\n%s\r\n", strlen(msg), msg);
        printf("0\r\n\r\n");
    }

    return 0;
}