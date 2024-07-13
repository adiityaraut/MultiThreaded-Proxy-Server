#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>
#include <semaphore.h>
#include <arpa/inet.h>
#include "proxy_parse.h"
#include <time.h>

#define PORT 8080
#define MAX_CACHE_SIZE 100
#define BUFFER_SIZE 1024

typedef struct CacheEntry {
    char url[BUFFER_SIZE];
    char response[BUFFER_SIZE];
    time_t lru_time;
} CacheEntry;

CacheEntry cache[MAX_CACHE_SIZE];
int cache_count = 0;
pthread_mutex_t cache_mutex;
sem_t cache_sem;

void error(const char *msg) {
    perror(msg);
    exit(1);
}

int find_in_cache(const char *url, char *response) {
    pthread_mutex_lock(&cache_mutex);
    for (int i = 0; i < cache_count; i++) {
        if (strcmp(cache[i].url, url) == 0) {
            strcpy(response, cache[i].response);
            cache[i].lru_time = time(NULL); // Update LRU time
            pthread_mutex_unlock(&cache_mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&cache_mutex);
    return 0;
}

void add_to_cache(const char *url, const char *response) {
    pthread_mutex_lock(&cache_mutex);
    if (cache_count < MAX_CACHE_SIZE) {
        strcpy(cache[cache_count].url, url);
        strcpy(cache[cache_count].response, response);
        cache[cache_count].lru_time = time(NULL);
        cache_count++;
    } else {
        // Simple cache replacement strategy: Replace the least recently used entry
        int lru_index = 0;
        for (int i = 1; i < cache_count; i++) {
            if (cache[i].lru_time < cache[lru_index].lru_time) {
                lru_index = i;
            }
        }
        strcpy(cache[lru_index].url, url);
        strcpy(cache[lru_index].response, response);
        cache[lru_index].lru_time = time(NULL);
    }
    pthread_mutex_unlock(&cache_mutex);
}

void handle_request(int newsockfd, ParsedRequest *req, char *buffer) {
    char response[BUFFER_SIZE];
    if (find_in_cache(req->host, response)) {
        printf("LRU Time Track Before: %ld\n", time(NULL));
        printf("URL found\n");
        printf("LRU Time Track After: %ld\n", time(NULL));
        printf("Data retrieved from the Cache\n");
        write(newsockfd, response, strlen(response));
    } else {
        printf("URL not found\n");
        struct hostent *server;
        struct sockaddr_in serv_addr;
        int sockfd, bytes, total_bytes = 0;

        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) error("ERROR opening socket");

        server = gethostbyname(req->host);
        if (server == NULL) error("ERROR, no such host");

        bzero((char *) &serv_addr, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        bcopy((char *)server->h_addr, (char *)&serv_addr.sin_addr.s_addr, server->h_length);
        serv_addr.sin_port = htons(80);

        if (connect(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
            error("ERROR connecting");

        write(sockfd, buffer, strlen(buffer));

        while ((bytes = read(sockfd, response + total_bytes, BUFFER_SIZE - total_bytes - 1)) > 0) {
            total_bytes += bytes;
        }
        response[total_bytes] = '\0';
        add_to_cache(req->host, response);
        write(newsockfd, response, total_bytes);
        close(sockfd);
    }
}

void *thread_fn(void *arg) {
    int newsockfd = *(int *)arg;
    free(arg);
    char buffer[BUFFER_SIZE];
    ParsedRequest *req;

    printf("Thread started for client socket: %d\n", newsockfd);

    bzero(buffer, BUFFER_SIZE);
    int bytes_read = read(newsockfd, buffer, BUFFER_SIZE - 1);
    if (bytes_read <= 0) {
        printf("Invalid buflen %d\n", bytes_read);
        close(newsockfd);
        return NULL;
    }
    buffer[bytes_read] = '\0';
    printf("Request received: %s\n", buffer);

    req = ParsedRequest_create();
    if (ParsedRequest_parse(req, buffer, strlen(buffer)) < 0) {
        printf("ParsedRequest_parse failed\n");
        ParsedRequest_destroy(req);
        close(newsockfd);
        return NULL;
    }
    printf("ParsedRequest created and parsed successfully\n");

    handle_request(newsockfd, req, buffer);
    ParsedRequest_destroy(req);
    close(newsockfd);
    printf("Client disconnected from socket: %d\n", newsockfd);
    return NULL;
}

int main(int argc, char *argv[]) {
    int sockfd, newsockfd, portno;
    socklen_t clilen;
    struct sockaddr_in serv_addr, cli_addr;

    pthread_mutex_init(&cache_mutex, NULL);
    sem_init(&cache_sem, 0, MAX_CACHE_SIZE);

    if (argc < 2) {
        fprintf(stderr, "ERROR, no port provided\n");
        exit(1);
    }
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    bzero((char *) &serv_addr, sizeof(serv_addr));
    portno = atoi(argv[1]);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(portno);

    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
        error("ERROR on binding");

    listen(sockfd, 5);
    clilen = sizeof(cli_addr);
    printf("Setting Proxy Server Port: %d\n", portno);
    printf("Binding on port: %d\n", portno);

    while (1) {
        newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
        if (newsockfd < 0)
            error("ERROR on accept");

        printf("Client is connected with port number: %d and IP address: %s\n", ntohs(cli_addr.sin_port), inet_ntoa(cli_addr.sin_addr));

        pthread_t thread;
        int *pclient = (int *)malloc(sizeof(int));
        *pclient = newsockfd;
        pthread_create(&thread, NULL, thread_fn, pclient);
        pthread_detach(thread);
    }
    close(sockfd);
    pthread_mutex_destroy(&cache_mutex);
    sem_destroy(&cache_sem);
    return 0;
}






