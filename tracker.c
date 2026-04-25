#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 3490
#define MAXLINE 4096

void handle_get(int sock, char *file){
    FILE *fp = fopen("testfile.track", "r");
    if(!fp){
        write(sock, "<REP GET BEGIN>\nERROR\n<REP GET END dummy>\n", 45);
        return;
    }

    char data[MAXLINE];
    int n = fread(data, 1, MAXLINE, fp);
    fclose(fp);

    char response[MAXLINE*2];
    sprintf(response,
        "<REP GET BEGIN>\n%.*s\n<REP GET END dummyMD5>\n",
        n, data);

    write(sock, response, strlen(response));
}

void handler(int sock){
    char buffer[MAXLINE];
    int n = read(sock, buffer, MAXLINE);
    buffer[n] = '\0';

    if(strstr(buffer, "GET")){
        char fname[100];
        sscanf(buffer, "GET %s", fname);
        handle_get(sock, fname);
    }
}

int main(){
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(sock, (struct sockaddr*)&addr, sizeof(addr));
    listen(sock, 5);

    printf("Tracker running...\n");

    while(1){
        int client = accept(sock, NULL, NULL);
        handler(client);
        close(client);
    }
}