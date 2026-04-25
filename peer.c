#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#define PORT 3490
#define CHUNK_SIZE 1024

pthread_mutex_t file_lock = PTHREAD_MUTEX_INITIALIZER;

/* ================= STRUCTS ================= */

typedef struct {
    char filename[100];
    int start;
    int end;
    char ip[50];
    int port;
} chunk_t;

typedef struct {
    char ip[50];
    int port;
    int start;
    int end;
    long timestamp;
} peer_info;

typedef struct {
    char filename[100];
    int filesize;
    char md5[50];
    peer_info peers[50];
    int peer_count;
} tracker_t;

/* ================= SELECT PEER ================= */

int select_peer(tracker_t *tracker, int start, int end){
    int best = -1;
    long best_time = -1;

    for(int i = 0; i < tracker->peer_count; i++){
        peer_info *p = &tracker->peers[i];

        if(p->start <= start && p->end >= end){
            if(p->timestamp > best_time){
                best_time = p->timestamp;
                best = i;
            }
        }
    }
    return best;
}

/* ================= PARSE TRACKER ================= */

void parse_tracker(char *filename, tracker_t *tracker){
    FILE *fp = fopen(filename, "r");
    if(!fp){
        printf("Error opening tracker file\n");
        exit(1);
    }

    char line[256];
    tracker->peer_count = 0;

    while(fgets(line, sizeof(line), fp)){
        line[strcspn(line, "\n")] = 0;

        if(line[0] == '#' || strlen(line) == 0) continue;

        if(strncmp(line, "Filename:", 9) == 0){
            sscanf(line, "Filename: %s", tracker->filename);
        }
        else if(strncmp(line, "Filesize:", 9) == 0){
            sscanf(line, "Filesize: %d", &tracker->filesize);
        }
        else if(strncmp(line, "MD5:", 4) == 0){
            sscanf(line, "MD5: %s", tracker->md5);
        }
        else {
            peer_info *p = &tracker->peers[tracker->peer_count];

            sscanf(line, "%[^:]:%d:%d:%d:%ld",
                   p->ip, &p->port, &p->start, &p->end, &p->timestamp);

            tracker->peer_count++;
        }
    }

    fclose(fp);
}

/* ================= DOWNLOAD THREAD ================= */

void *download_chunk(void *arg){
    chunk_t *chunk = (chunk_t *)arg;

    int sock = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(chunk->port);
    server_addr.sin_addr.s_addr = inet_addr(chunk->ip);

    if(connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0){
        printf("Connection failed for %d-%d\n", chunk->start, chunk->end);
        free(chunk);
        return NULL;
    }

    char request[100];
    sprintf(request, "GET %s %d %d", chunk->filename, chunk->start, chunk->end);

    write(sock, request, strlen(request));

    char buffer[CHUNK_SIZE];
    int n = read(sock, buffer, CHUNK_SIZE);

    if(n <= 0){
        printf("Read failed for chunk %d-%d\n", chunk->start, chunk->end);
        close(sock);
        free(chunk);
        return NULL;
    }

    pthread_mutex_lock(&file_lock);

    FILE *out = fopen("output.txt", "r+");
    if(!out) out = fopen("output.txt", "w+");

    fseek(out, chunk->start, SEEK_SET);
    fwrite(buffer, 1, n, out);
    fclose(out);

    pthread_mutex_unlock(&file_lock);

    close(sock);

    printf("Downloaded %d-%d\n", chunk->start, chunk->end);

    free(chunk);
    return NULL;
}

/* ================= DOWNLOAD FILE ================= */

void download_file(char *filename, int filesize, tracker_t tracker){
    int num_chunks = (filesize + CHUNK_SIZE - 1) / CHUNK_SIZE;
    pthread_t threads[num_chunks];

    FILE *out = fopen("output.txt", "w");
    fseek(out, filesize - 1, SEEK_SET);
    fputc('\0', out);
    fclose(out);

    for(int i = 0; i < num_chunks; i++){
        int start = i * CHUNK_SIZE;
        int end = start + CHUNK_SIZE;
        if(end > filesize) end = filesize;

        int p = select_peer(&tracker, start, end);

        if(p == -1){
            printf("No peer for %d-%d\n", start, end);
            continue;
        }

        chunk_t *chunk = malloc(sizeof(chunk_t));
        strcpy(chunk->filename, filename);
        chunk->start = start;
        chunk->end = end;
        strcpy(chunk->ip, tracker.peers[p].ip);
        chunk->port = tracker.peers[p].port;

        printf("Chunk %d-%d → %s:%d\n",
               start, end, chunk->ip, chunk->port);

        pthread_create(&threads[i], NULL, download_chunk, chunk);
    }

    for(int i = 0; i < num_chunks; i++){
        pthread_join(threads[i], NULL);
    }

    printf("Download Complete!\n");
}

/* ================= PEER SERVER ================= */

void *peer_server(void *arg){
    int server_sock, client_sock;
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    server_sock = socket(AF_INET, SOCK_STREAM, 0);

    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if(bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0){
        printf("Bind failed\n");
        exit(1);
    }

    listen(server_sock, 5);

    printf("Peer server listening...\n");

    while(1){
        client_sock = accept(server_sock, (struct sockaddr*)&addr, &addr_len);

        char buffer[1024];
        int n = read(client_sock, buffer, 1024);
        if(n <= 0){
            close(client_sock);
            continue;
        }

        buffer[n] = '\0';

        if(strstr(buffer,"GET")){
            char filename[100];
            int start, end;

            sscanf(buffer, "GET %s %d %d", filename, &start, &end);

            FILE *fp = fopen(filename, "r");
            if(!fp){
                close(client_sock);
                continue;
            }

            fseek(fp, start, SEEK_SET);

            char chunk[CHUNK_SIZE];
            int bytes = fread(chunk, 1, end - start, fp);

            write(client_sock, chunk, bytes);
            fclose(fp);
        }

        close(client_sock);
    }
}

/* ================= MAIN ================= */

int main(){
    pthread_t tid;
    pthread_create(&tid, NULL, peer_server, NULL);

    sleep(1); // allow server to start

    tracker_t tracker;
    parse_tracker("test.track", &tracker);

    download_file(tracker.filename, tracker.filesize, tracker);

    return 0;
}