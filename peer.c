#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#define PORT 3490
#define MAXLINE 4096

void *peer_server(void *arg){
    int server_sock,client_sock;
    struct sockaddr_in addr;
    socklen_t addr_len= sizeof(addr);

    server_sock = socket(AF_INET, SOCK_STREAM, 0);

    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    bind(server_sock, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_sock, 5);

    printf("Peer server is listenning...\n");

    while(1){
        client_sock = accept(server_sock,(struct sockaddr*)&addr, &addr_len);

        char buffer[1024];
        int n = read(client_sock, buffer, 1024);
        buffer[n] = '\0';

        if(strstr(buffer,"GET")){
            char filename[100];
            int start, end;

            sscanf(buffer, "GET %s %d %d", filename, &start, &end);

            if((end - start) > 1024){
                write(client_sock, "<GET INVALID>\n", 15);

            } else{
                FILE *fp = fopen(filename, "r");
                fseek(fp,start, SEEK_SET);

                char chunk[1024];
                int bytes = fread(chunk, 1, end - start, fp);
                write(client_sock,chunk, bytes);
                fclose(fp);
            }
        }
        printf("Received from peer; %s\n", buffer);
        close(client_sock);
    }
}

int main(int argc, char *argv[]) {
    pthread_t tid;
    pthread_create(&tid, NULL, peer_server, NULL);

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