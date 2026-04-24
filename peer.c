#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#define PORT 3490
#define MAXLINE 4096
#define CHUNK_SIZE 1024

typedef struct {
    char filename[100];
    int start;
    int end;
} chunk_t;

void *download_chunk(void *arg){
    chunk_t *chunk = (chunk_t *)arg;

    int sock = socket(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr));

    char request[100];
    sprintf(request, "GET %s %d %d", chunk->filename,chunk->start,chunk->end);

    write(sock,request,strlen(request));

    char buffer[1024];
    int n = read(sock,buffer,1024);

    FILE *out = fopen("output.txt","r+");
    fseek(out,chunk->start,SEEK_SET);
    fwrite(buffer,1,n,out);
    fclose(out);

    close(sock);

    printf("Thread Downlaoded bytes %d-%d\n",chunk->start,chunk->end);
    free(chunk);
    return NULL;
}

void download_file(char *filename, int filesize){
    int num_chunks = (filesize + CHUNK_SIZE -1) / CHUNK_SIZE;
    pthread_t threads[num_chunks];

    FILE *out = fopen("output.txt", "w");
    fseek(out,filesize - 1,SEEK_SET);
    fputc('\0',out);
    fclose(out);
    
    for(int i = 0; i <= num_chunks;i++){
        int start = i * CHUNK_SIZE;
        int end = start + CHUNK_SIZE;
        if(end > filesize) end = filesize;

        chunk_t *chunk = malloc(sizeof(chunk_t));
        strcpy(chunk->filename,filename);
        chunk->start = start;
        chunk->end = end;

        pthread_create(&threads[i],NULL,download_chunk,chunk);
    }
    for(int i = 0; i < num_chunks;i++){
        pthread_join(threads[i],NULL);
    }
    printf("Downlaod Complete!\n");


}

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
        printf("Received from peer: %s\n", buffer);
        close(client_sock);
    }
}

int main(int argc, char *argv[]) {
    pthread_t tid;
    pthread_create(&tid, NULL, peer_server, NULL);
    sleep(1); // give server time to start
     download_file("test.txt", 50);
    return 0;
}