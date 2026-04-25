#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_TRACKER_PORT 3490
#define DEFAULT_TRACKER_DIR "torrents"
#define DEFAULT_DEAD_PEER_SECS 1800
#define MAXLINE 4096
#define MAX_TRACKER_BODY 65536
#define MAX_PEERS 256

typedef struct {
    int port;
    int dead_peer_secs;
    char shared_dir[PATH_MAX];
} tracker_config_t;

typedef struct {
    int sock;
    tracker_config_t *config;
} worker_arg_t;

typedef struct {
    char ip[64];
    int port;
    long start;
    long end;
    long timestamp;
} peer_entry_t;

typedef struct {
    char filename[256];
    long filesize;
    char description[256];
    char md5[64];
    peer_entry_t peers[MAX_PEERS];
    int peer_count;
} tracker_record_t;

static pthread_mutex_t tracker_lock = PTHREAD_MUTEX_INITIALIZER;

static void trim_newline(char *text) {
    if (text == NULL) {
        return;
    }
    text[strcspn(text, "\r\n")] = '\0';
}

static int ensure_dir(const char *path) {
    struct stat st;

    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }

    return mkdir(path, 0777);
}

static int first_nonempty_line(const char *path, int index, char *out, size_t out_size) {
    FILE *fp;
    char line[PATH_MAX];
    int current = 0;

    fp = fopen(path, "r");
    if (fp == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        trim_newline(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        if (current == index) {
            snprintf(out, out_size, "%s", line);
            fclose(fp);
            return 0;
        }
        current++;
    }

    fclose(fp);
    return -1;
}

static void load_config(tracker_config_t *config) {
    char value[PATH_MAX];

    config->port = DEFAULT_TRACKER_PORT;
    config->dead_peer_secs = DEFAULT_DEAD_PEER_SECS;
    snprintf(config->shared_dir, sizeof(config->shared_dir), "%s", DEFAULT_TRACKER_DIR);

    if (first_nonempty_line("sconfig", 0, value, sizeof(value)) == 0) {
        config->port = atoi(value);
    }
    if (first_nonempty_line("sconfig", 1, value, sizeof(value)) == 0) {
        snprintf(config->shared_dir, sizeof(config->shared_dir), "%s", value);
    }
    if (first_nonempty_line("sconfig", 2, value, sizeof(value)) == 0) {
        config->dead_peer_secs = atoi(value);
    }
}

static int compute_file_md5(const char *path, char *out, size_t out_size) {
    FILE *pipe;
    char command[PATH_MAX + 64];
    char line[256];
    int status = -1;

#ifdef __APPLE__
    snprintf(command, sizeof(command), "md5 -q \"%s\"", path);
#else
    snprintf(command, sizeof(command), "md5sum \"%s\"", path);
#endif

    pipe = popen(command, "r");
    if (pipe == NULL) {
        return -1;
    }

    if (fgets(line, sizeof(line), pipe) != NULL) {
#ifdef __APPLE__
        trim_newline(line);
        snprintf(out, out_size, "%s", line);
#else
        if (sscanf(line, "%63s", out) == 1) {
            status = 0;
        }
#endif
    }

#ifdef __APPLE__
    if (line[0] != '\0') {
        status = 0;
    }
#endif

    pclose(pipe);
    return status;
}

static void tracker_path(const tracker_config_t *config, const char *filename, char *out, size_t out_size) {
    snprintf(out, out_size, "%s/%s.track", config->shared_dir, filename);
}

static int load_tracker_record(const char *path, tracker_record_t *record) {
    FILE *fp;
    char line[1024];

    memset(record, 0, sizeof(*record));
    fp = fopen(path, "r");
    if (fp == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        trim_newline(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        if (strncmp(line, "Filename:", 9) == 0) {
            snprintf(record->filename, sizeof(record->filename), "%s", line + 10);
        } else if (strncmp(line, "Filesize:", 9) == 0) {
            record->filesize = atol(line + 10);
        } else if (strncmp(line, "Description:", 12) == 0) {
            snprintf(record->description, sizeof(record->description), "%s", line + 13);
        } else if (strncmp(line, "MD5:", 4) == 0) {
            snprintf(record->md5, sizeof(record->md5), "%s", line + 5);
        } else if (record->peer_count < MAX_PEERS) {
            peer_entry_t *peer = &record->peers[record->peer_count];
            if (sscanf(line, "%63[^:]:%d:%ld:%ld:%ld",
                       peer->ip,
                       &peer->port,
                       &peer->start,
                       &peer->end,
                       &peer->timestamp) == 5) {
                record->peer_count++;
            }
        }
    }

    fclose(fp);
    return 0;
}

static int save_tracker_record(const char *path, const tracker_record_t *record) {
    FILE *fp;
    int i;

    fp = fopen(path, "w");
    if (fp == NULL) {
        return -1;
    }

    fprintf(fp, "Filename: %s\n", record->filename);
    fprintf(fp, "Filesize: %ld\n", record->filesize);
    fprintf(fp, "Description: %s\n", record->description[0] != '\0' ? record->description : "-");
    fprintf(fp, "MD5: %s\n", record->md5);
    fprintf(fp, "#list of peers follows next\n");

    for (i = 0; i < record->peer_count; i++) {
        fprintf(fp, "%s:%d:%ld:%ld:%ld\n",
                record->peers[i].ip,
                record->peers[i].port,
                record->peers[i].start,
                record->peers[i].end,
                record->peers[i].timestamp);
    }

    fclose(fp);
    return 0;
}

static void send_text(int sock, const char *text) {
    write(sock, text, strlen(text));
}

static void prune_dead_peers(tracker_record_t *record, int dead_peer_secs) {
    time_t now = time(NULL);
    int kept = 0;
    int i;

    for (i = 0; i < record->peer_count; i++) {
        if ((now - record->peers[i].timestamp) <= dead_peer_secs) {
            if (kept != i) {
                record->peers[kept] = record->peers[i];
            }
            kept++;
        }
    }

    record->peer_count = kept;
}

static void handle_list(int sock, const tracker_config_t *config) {
    DIR *dir;
    struct dirent *entry;
    char response[MAX_TRACKER_BODY];
    char path[PATH_MAX];
    tracker_record_t record;
    int count = 0;
    int index = 1;

    response[0] = '\0';
    dir = opendir(config->shared_dir);
    if (dir == NULL) {
        send_text(sock, "<REP LIST 0>\n<REP LIST END>\n");
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len > 6 && strcmp(entry->d_name + len - 6, ".track") == 0) {
            count++;
        }
    }
    rewinddir(dir);

    snprintf(response, sizeof(response), "<REP LIST %d>\n", count);

    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        char base[256];
        if (!(len > 6 && strcmp(entry->d_name + len - 6, ".track") == 0)) {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", config->shared_dir, entry->d_name);
        if (load_tracker_record(path, &record) != 0) {
            continue;
        }

        snprintf(base, sizeof(base), "%s", entry->d_name);
        base[len - 6] = '\0';

        snprintf(response + strlen(response),
                 sizeof(response) - strlen(response),
                 "<%d %s %ld %s>\n",
                 index++,
                 base,
                 record.filesize,
                 record.md5);
    }

    snprintf(response + strlen(response),
             sizeof(response) - strlen(response),
             "<REP LIST END>\n");

    closedir(dir);
    send_text(sock, response);
}

static void handle_get(int sock, const tracker_config_t *config, const char *request_name) {
    FILE *fp;
    char path[PATH_MAX];
    char md5[64] = {0};
    char body[MAX_TRACKER_BODY];
    char response[MAX_TRACKER_BODY + 256];
    size_t len;

    snprintf(path, sizeof(path), "%s/%s", config->shared_dir, request_name);
    fp = fopen(path, "r");
    if (fp == NULL) {
        send_text(sock, "<REP GET BEGIN>\nERROR\n<REP GET END invalid>\n");
        return;
    }

    len = fread(body, 1, sizeof(body) - 1, fp);
    body[len] = '\0';
    fclose(fp);

    if (compute_file_md5(path, md5, sizeof(md5)) != 0) {
        snprintf(md5, sizeof(md5), "unknown");
    }

    snprintf(response, sizeof(response),
             "<REP GET BEGIN>\n%s<REP GET END %s>\n",
             body,
             md5);
    send_text(sock, response);
}

static void handle_create(int sock, const tracker_config_t *config, char *request) {
    char filename[256];
    long filesize;
    char description[256];
    char md5[64];
    char ip[64];
    int port;
    char path[PATH_MAX];
    tracker_record_t record;

    if (sscanf(request, "createtracker %255s %ld %255s %63s %63s %d",
               filename, &filesize, description, md5, ip, &port) != 6) {
        send_text(sock, "<createtracker fail>\n");
        return;
    }

    tracker_path(config, filename, path, sizeof(path));

    pthread_mutex_lock(&tracker_lock);

    if (access(path, F_OK) == 0) {
        pthread_mutex_unlock(&tracker_lock);
        send_text(sock, "<createtracker ferr>\n");
        return;
    }

    memset(&record, 0, sizeof(record));
    snprintf(record.filename, sizeof(record.filename), "%s", filename);
    record.filesize = filesize;
    snprintf(record.description, sizeof(record.description), "%s", description);
    snprintf(record.md5, sizeof(record.md5), "%s", md5);
    record.peer_count = 1;
    snprintf(record.peers[0].ip, sizeof(record.peers[0].ip), "%s", ip);
    record.peers[0].port = port;
    record.peers[0].start = 0;
    record.peers[0].end = filesize > 0 ? filesize - 1 : 0;
    record.peers[0].timestamp = time(NULL);

    if (save_tracker_record(path, &record) != 0) {
        pthread_mutex_unlock(&tracker_lock);
        send_text(sock, "<createtracker fail>\n");
        return;
    }

    pthread_mutex_unlock(&tracker_lock);
    send_text(sock, "<createtracker succ>\n");
}

static void handle_update(int sock, const tracker_config_t *config, char *request) {
    char filename[256];
    long start;
    long end;
    char ip[64];
    int port;
    char path[PATH_MAX];
    tracker_record_t record;
    int i;
    int found = 0;

    if (sscanf(request, "updatetracker %255s %ld %ld %63s %d",
               filename, &start, &end, ip, &port) != 5) {
        send_text(sock, "<updatetracker invalid fail>\n");
        return;
    }

    tracker_path(config, filename, path, sizeof(path));

    pthread_mutex_lock(&tracker_lock);

    if (load_tracker_record(path, &record) != 0) {
        pthread_mutex_unlock(&tracker_lock);
        dprintf(sock, "<updatetracker %s ferr>\n", filename);
        return;
    }

    prune_dead_peers(&record, config->dead_peer_secs);

    for (i = 0; i < record.peer_count; i++) {
        if (strcmp(record.peers[i].ip, ip) == 0 && record.peers[i].port == port) {
            record.peers[i].start = start;
            record.peers[i].end = end;
            record.peers[i].timestamp = time(NULL);
            found = 1;
            break;
        }
    }

    if (!found) {
        if (record.peer_count >= MAX_PEERS) {
            pthread_mutex_unlock(&tracker_lock);
            dprintf(sock, "<updatetracker %s fail>\n", filename);
            return;
        }

        snprintf(record.peers[record.peer_count].ip, sizeof(record.peers[record.peer_count].ip), "%s", ip);
        record.peers[record.peer_count].port = port;
        record.peers[record.peer_count].start = start;
        record.peers[record.peer_count].end = end;
        record.peers[record.peer_count].timestamp = time(NULL);
        record.peer_count++;
    }

    if (save_tracker_record(path, &record) != 0) {
        pthread_mutex_unlock(&tracker_lock);
        dprintf(sock, "<updatetracker %s fail>\n", filename);
        return;
    }

    pthread_mutex_unlock(&tracker_lock);
    dprintf(sock, "<updatetracker %s succ>\n", filename);
}

static void *worker_main(void *arg) {
    worker_arg_t *worker = (worker_arg_t *)arg;
    char buffer[MAXLINE];
    int bytes;

    bytes = read(worker->sock, buffer, sizeof(buffer) - 1);
    if (bytes <= 0) {
        close(worker->sock);
        free(worker);
        return NULL;
    }

    buffer[bytes] = '\0';
    trim_newline(buffer);

    if (strcmp(buffer, "REQ LIST") == 0) {
        handle_list(worker->sock, worker->config);
    } else if (strncmp(buffer, "GET ", 4) == 0) {
        handle_get(worker->sock, worker->config, buffer + 4);
    } else if (strncmp(buffer, "createtracker ", 14) == 0) {
        handle_create(worker->sock, worker->config, buffer);
    } else if (strncmp(buffer, "updatetracker ", 14) == 0) {
        handle_update(worker->sock, worker->config, buffer);
    } else {
        send_text(worker->sock, "<invalid command>\n");
    }

    close(worker->sock);
    free(worker);
    return NULL;
}

int main(void) {
    int server_sock;
    int opt = 1;
    struct sockaddr_in addr;
    tracker_config_t config;

    load_config(&config);
    if (ensure_dir(config.shared_dir) != 0) {
        fprintf(stderr, "Unable to create tracker directory %s: %s\n", config.shared_dir, strerror(errno));
        return 1;
    }

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        return 1;
    }

    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_sock);
        return 1;
    }

    if (listen(server_sock, 16) < 0) {
        perror("listen");
        close(server_sock);
        return 1;
    }

    printf("Tracker running on port %d using %s\n", config.port, config.shared_dir);

    while (1) {
        int client_sock;
        pthread_t tid;
        worker_arg_t *worker;

        client_sock = accept(server_sock, NULL, NULL);
        if (client_sock < 0) {
            continue;
        }

        worker = malloc(sizeof(*worker));
        if (worker == NULL) {
            close(client_sock);
            continue;
        }

        worker->sock = client_sock;
        worker->config = &config;

        if (pthread_create(&tid, NULL, worker_main, worker) == 0) {
            pthread_detach(tid);
        } else {
            close(client_sock);
            free(worker);
        }
    }

    close(server_sock);
    return 0;
}
