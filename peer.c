#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#define PORT 3490
#define MAXLINE 4096


int main(int argc, char *argv[]) {
    struct sockaddr_in server_addr;
    int sockid;
    char buffer[MAXLINE];

    sockid = socket(AF_INET, SOCK_STREAM, 0);

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sockid, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        printf("Cannot connect to server\n");
        exit(0);
    }

    printf("Enter command:\n");
    fgets(buffer, MAXLINE, stdin);

    write(sockid, buffer, strlen(buffer));

    int n = read(sockid, buffer, MAXLINE);
    buffer[n] = '\0';

    printf("Server response:\n%s\n", buffer);

    // Save file if GET
    if (strstr(buffer, "REP GET BEGIN")) {
        FILE *fp = fopen("downloaded.track", "w");

        char *start = strstr(buffer, "REP GET BEGIN\n") + strlen("REP GET BEGIN\n");
        char *end = strstr(buffer, "\nREP GET END");

        if (start && end) {
            fwrite(start, 1, end - start, fp);
        }

        fclose(fp);
        printf("File saved as downloaded.track\n");
    }

    close(sockid);
    return 0;
}