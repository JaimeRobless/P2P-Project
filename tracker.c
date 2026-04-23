#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 3490
#define MAXLINE 4096

int sockid, sock_child, clilen;
char read_msg[MAXLINE];

void handle_list_req(int sock) {
    char response[] =
        "REP LIST 1\n"
        "1 testfile 100 dummyMD5\n"
        "REP LIST END\n";
    write(sock, response, strlen(response));
}

void handle_get_req(int sock, char *fname) {
    FILE *fp = fopen("testfile.track", "r");
    if (!fp) {
        write(sock, "ERROR\n", 6);
        return;
    }

    char file_data[MAXLINE];
    fread(file_data, 1, MAXLINE, fp);
    fclose(fp);

    char response[MAXLINE * 2];
    sprintf(response,
        "REP GET BEGIN\n%s\nREP GET END dummyMD5\n",
        file_data);

    write(sock, response, strlen(response));
}

void handle_createtracker_req(int sock) {
    FILE *fp = fopen("testfile.track", "w");
    fprintf(fp, "Filename: testfile\nFilesize: 100\nMD5: dummy\n");
    fclose(fp);

    write(sock, "createtracker succ\n", 20);
}

void handle_updatetracker_req(int sock) {
    write(sock, "updatetracker succ\n", 20);
}

void peer_handler(int sock_child) {
    int length = read(sock_child, read_msg, MAXLINE);
    read_msg[length] = '\0';

    printf("Received: %s\n", read_msg);

    if (strstr(read_msg, "REQ LIST")) {
        handle_list_req(sock_child);
    }
    else if (strstr(read_msg, "GET")) {
        handle_get_req(sock_child, "testfile.track");
    }
    else if (strstr(read_msg, "createtracker")) {
        handle_createtracker_req(sock_child);
    }
    else if (strstr(read_msg, "updatetracker")) {
        handle_updatetracker_req(sock_child);
    }
}

int main() {
    pid_t pid;
    struct sockaddr_in server_addr, client_addr;

    sockid = socket(AF_INET, SOCK_STREAM, 0);

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    bind(sockid, (struct sockaddr *)&server_addr, sizeof(server_addr));

    printf("Tracker SERVER READY...\n");

    listen(sockid, 5);

    clilen = sizeof(client_addr);

    while (1) {
        sock_child = accept(sockid, (struct sockaddr *)&client_addr, (socklen_t *)&clilen);

        if ((pid = fork()) == 0) {
            close(sockid);
            peer_handler(sock_child);
            close(sock_child);
            exit(0);
        }

        close(sock_child);
    }
}