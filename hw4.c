#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <assert.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <semaphore.h>

#define exit(N) {fflush(stdout); fflush(stderr); _exit(N); }
#define MAX 1024
#define MAX_EVENTS 10
#define NUM_THREADS 4

static const char* httpErrors[] = {"HTTP/1.1 404 Not Found\r\n\r\n","HTTP/1.1 400 Bad Request\r\n\r\n", "HTTP/1.1 413 Request Entity Too Large\r\n\r\n"};

struct client {
    int clientfd;
    int filefd;
    char clientBuff[MAX * 4];
    char postBody[MAX + 1];
    int contentLength;
    char serverHeader[MAX];
    char serverBody[MAX + 1];
    char response[2 * MAX];
    char fileBuff[MAX + 1];
    int file;
    int num;
    ssize_t RSIZE;
};

static struct client clients[205];
static int nextClient = 0;

static int requests = 0;
static int headerBytes = 0;
static int bodyBytes = 0;
static int errors = 0;
static int errorBytes = 0;

static sem_t client_sem;
static pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

// Function prototypes
static int get_port(void);
static int Socket(int namespace, int style, int protocol);
static void Bind(int sockfd, const struct sockaddr * addr, socklen_t addrlen);
static void Listen(int sockfd, int backlog);
static int Accept(int listenfd, struct sockaddr* addr, socklen_t* addrlen);
static int acceptClient(int listenfd);
int SplitRequest(struct client* client, char** requestLine, char** headers, char** body);
int SplitRequestLine(char* requestLine, char** method, char** url);
void Get(struct client* client, char* url, char* headers, char* body);
void Post(struct client* client, char* url, char* headers, char* body);
void Receive(struct client* client);
void* worker_thread(void* arg);

// Main function
int main(int argc, char * argv[]) {
    int port = get_port();
    printf("Using port %d\n", port);
    printf("PID: %d\n", getpid());

    // Initialize semaphore
    sem_init(&client_sem, 0, 0);

    // Create worker threads
    pthread_t threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, worker_thread, NULL);
    }

    // Set up server socket
    int listenfd = Socket(AF_INET, SOCK_STREAM, 0);
    static struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);

    int optval = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    Bind(listenfd, (struct sockaddr*)&server, sizeof(server));
    Listen(listenfd, 10);

    // Main loop: accept clients and add them to the queue
    while (1) {
        int clientfd = acceptClient(listenfd);
        clients[nextClient].clientfd = clientfd;
        strcpy(clients[nextClient].postBody, "<empty>");
        clients[nextClient].contentLength = 7;
        clients[nextClient].RSIZE = 1;
        clients[nextClient].filefd = -1;
        clients[nextClient].file = 0;

        nextClient++;
        sem_post(&client_sem);
    }

    return 0;
}

// Worker thread function
void* worker_thread(void* arg) {
    while (1) {
        sem_wait(&client_sem);
        int client_index = nextClient - 1;
        Receive(&clients[client_index]);
        close(clients[client_index].clientfd);
        if (clients[client_index].filefd != -1) {
            close(clients[client_index].filefd);
        }
    }
    return NULL;
}

// Function to get port number from file
static int get_port(void) {
    int fd = open("port.txt", O_RDONLY);
    if (fd < 0) {
        perror("Could not open port.txt");
        exit(1);
    }

    char buffer[32];
    int r = read(fd, buffer, sizeof(buffer));
    if (r < 0) {
        perror("Could not read port.txt");
        exit(1);
    }

    return atoi(buffer);
}

// Wrapper function for socket()
static int Socket(int namespace, int style, int protocol) {
    int fd = socket(namespace, style, protocol);
    if (fd < 0) {
        perror("Socket error");
        exit(1);
    }
    return fd;
}

// Wrapper function for bind()
static void Bind(int sockfd, const struct sockaddr * addr, socklen_t addrlen) {
    if (bind(sockfd, addr, addrlen) < 0) {
        perror("Bind error");
        exit(1);
    }
}

// Wrapper function for listen()
static void Listen(int sockfd, int backlog) {
    if (listen(sockfd, backlog) < 0) {
        perror("Listen error");
        exit(1);
    }
}

// Wrapper function for accept()
static int Accept(int listenfd, struct sockaddr* addr, socklen_t* addrlen) {
    int fd = accept(listenfd, addr, addrlen);
    if (fd < 0) {
        perror("Accept error");
        exit(1);
    }
    return fd;
}

// Function to accept a client connection
static int acceptClient(int listenfd) {
    static struct sockaddr_in client;
    static socklen_t csize = 0;
    memset(&client, 0x00, sizeof(client));
    int clientfd = Accept(listenfd, (struct sockaddr*)&client, &csize);
    return clientfd;
}

// Function to split the HTTP request into its components
int SplitRequest(struct client* client, char** requestLine, char** headers, char** body) {
    char* crlf = strstr((*client).clientBuff, "\r\n");
    if (crlf == NULL)
        return 0;

    *crlf = '\0';
    *requestLine = (*client).clientBuff;
    *headers = (crlf + 2);

    crlf = strstr(*headers, "\r\n\r\n");
    if (crlf != NULL) {
        *crlf = '\0';
        *body = (crlf + 4);
    } else {
        *body = *headers;
        if (!strcmp("\r\n", *headers))
            return 1;
        return 0;
    }

    return 1;
}

// Function to split the request line into method and URL
int SplitRequestLine(char* requestLine, char** method, char** url) {
    char* del = strchr(requestLine, ' ');
    if (del == NULL) {
        return 0;
    }
    *del = '\0';

    *method = requestLine;
    *url = (del + 1);

    del = strchr(*url, ' ');
    if (del == NULL) {
        return 0;
    }
    *del = '\0';

    return 1;
}

// Function to handle GET requests
void Get(struct client* client, char* url, char* headers, char* body) {
    pthread_mutex_lock(&stats_mutex);
    requests++;
    pthread_mutex_unlock(&stats_mutex);

    if (!strcmp("/ping", url)) {
        snprintf((*client).serverHeader, MAX, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", strlen("pong"));
        snprintf((*client).serverBody, MAX, "%s", "pong");
        snprintf((*client).response, 2 * MAX, "%s%s", (*client).serverHeader, (*client).serverBody);
        send((*client).clientfd, (*client).response, strlen((*client).response), 0);

        pthread_mutex_lock(&stats_mutex);
        headerBytes += strlen((*client).serverHeader);
        bodyBytes += strlen((*client).serverBody);
        pthread_mutex_unlock(&stats_mutex);
    }
    else if (!strcmp("/echo", url)) {
        int size = strlen(headers);

        if (size > MAX) {
            pthread_mutex_lock(&stats_mutex);
            errorBytes += strlen(httpErrors[2]);
            errors++;
            pthread_mutex_unlock(&stats_mutex);
            send((*client).clientfd, httpErrors[2], strlen(httpErrors[2]), 0);
            return;
        }

        snprintf((*client).serverHeader, MAX, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", strlen(headers));
        snprintf((*client).serverBody, MAX + 1, "%s", headers);
        snprintf((*client).response, 2 * MAX, "%s%s", (*client).serverHeader, (*client).serverBody);
        send((*client).clientfd, (*client).response, strlen((*client).response), 0);

        pthread_mutex_lock(&stats_mutex);
        headerBytes += strlen((*client).serverHeader);
        bodyBytes += strlen((*client).serverBody);
        pthread_mutex_unlock(&stats_mutex);
    }
    else if (!strcmp("/read", url)) {
        snprintf((*client).serverHeader, MAX, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", (*client).contentLength);
        snprintf((*client).response, 2 * MAX, "%s", (*client).serverHeader);

        pthread_mutex_lock(&stats_mutex);
        headerBytes += strlen((*client).serverHeader);
        bodyBytes += (*client).contentLength;
        pthread_mutex_unlock(&stats_mutex);

        int before = strlen((*client).serverHeader);
        memcpy((*client).response + strlen((*client).response), (*client).postBody, (*client).contentLength);
        send((*client).clientfd, (*client).response, before + (*client).contentLength, 0);
    }
    else if (!strcmp("/stats", url)) {
        char statsBody[MAX + 1];
        pthread_mutex_lock(&stats_mutex);
        snprintf(statsBody, MAX + 1, "Requests: %d\nHeader bytes: %d\nBody bytes: %d\nErrors: %d\nError bytes: %d",
                 requests, headerBytes, bodyBytes, errors, errorBytes);
        pthread_mutex_unlock(&stats_mutex);

        snprintf((*client).serverHeader, MAX, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", strlen(statsBody));
        snprintf((*client).response, 2 * MAX, "%s%s", (*client).serverHeader, statsBody);
        send((*client).clientfd, (*client).response, strlen((*client).response), 0);

        pthread_mutex_lock(&stats_mutex);
        headerBytes += strlen((*client).serverHeader);
        bodyBytes += strlen(statsBody);
        pthread_mutex_unlock(&stats_mutex);
    }
    else {
        if ((*client).file == 0) {
            (*client).filefd = open((url + 1), O_RDONLY);
            if ((*client).filefd < 0) {
                send((*client).clientfd, httpErrors[0], strlen(httpErrors[0]), 0);
                pthread_mutex_lock(&stats_mutex);
                errorBytes += strlen(httpErrors[0]);
                errors++;
                pthread_mutex_unlock(&stats_mutex);
                return;
            }

            struct stat fileStat;
            if (fstat((*client).filefd, &fileStat) == -1) {
                send((*client).clientfd, httpErrors[0], strlen(httpErrors[0]), 0);
                (*client).filefd = -1;
                pthread_mutex_lock(&stats_mutex);
                errorBytes += strlen(httpErrors[0]);
                errors++;
                pthread_mutex_unlock(&stats_mutex);
                return;
            }

            if (S_ISDIR(fileStat.st_mode)) {
                send((*client).clientfd, httpErrors[0], strlen(httpErrors[0]), 0);
                (*client).filefd = -1;
                pthread_mutex_lock(&stats_mutex);
                errorBytes += strlen(httpErrors[0]);
                errors++;
                pthread_mutex_unlock(&stats_mutex);
                return;
            }

            (*client).file = 1;

            snprintf((*client).serverHeader, MAX, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", fileStat.st_size);
            snprintf((*client).response, 2 * MAX, "%s", (*client).serverHeader);
            send((*client).clientfd, (*client).response, strlen((*client).response), 0);

            pthread_mutex_lock(&stats_mutex);
            headerBytes += strlen((*client).serverHeader);
            bodyBytes += fileStat.st_size;
            pthread_mutex_unlock(&stats_mutex);
        }

        (*client).num = read((*client).filefd, (*client).fileBuff, MAX);

        if ((*client).num != 0)
            send((*client).clientfd, (*client).fileBuff, (*client).num, 0);
        else
            (*client).file = 0;
    }
}

// Function to handle POST requests
void Post(struct client* client, char* url, char* headers, char* body) {
    pthread_mutex_lock(&stats_mutex);
    requests++;
    pthread_mutex_unlock(&stats_mutex);

    if (!strcmp("/write", url)) {
        char* start = strstr(headers, "Content-Length: ");
        start += (strlen("Content-Length: "));
        char* end = strstr(start, "\r\n");
        if (end == NULL)
            end = strchr(start, '\0');

        *end = '\0';
        int origLength = (*client).contentLength;
        (*client).contentLength =  atoi(start);

        if ((*client).contentLength > MAX) {
            (*client).contentLength = origLength;
            send((*client).clientfd, httpErrors[2], strlen(httpErrors[2]), 0);
            pthread_mutex_lock(&stats_mutex);
            errorBytes += strlen(httpErrors[2]);
            errors++;
            pthread_mutex_unlock(&stats_mutex);
            return;
        }

        memcpy((*client).postBody, body, (*client).contentLength);
        (*client).postBody[(*client).contentLength] = '\0';

        snprintf((*client).serverHeader, MAX, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", (*client).contentLength);
        snprintf((*client).response, 2 * MAX, "%s", (*client).serverHeader);

        pthread_mutex_lock(&stats_mutex);
        headerBytes += strlen((*client).serverHeader);
        bodyBytes += (*client).contentLength;
        pthread_mutex_unlock(&stats_mutex);

        int before = strlen((*client).serverHeader);
        memcpy((*client).response + strlen((*client).response), (*client).postBody, (*client).contentLength);
        send((*client).clientfd, (*client).response, before + (*client).contentLength, 0);
    } else {
        send((*client).clientfd, httpErrors[1], strlen(httpErrors[1]), 0);
        pthread_mutex_lock(&stats_mutex);
        errorBytes += strlen(httpErrors[1]);
        errors++;
        pthread_mutex_unlock(&stats_mutex);
    }
}

// Function to receive and process client requests
void Receive(struct client* client) {
    (*client).RSIZE = recv((*client).clientfd, (*client).clientBuff, MAX * 4, 0);
    if ((*client).RSIZE == 0) {
        return;
    }

    (*client).clientBuff[(*client).RSIZE] = '\0';

    char* requestLine = NULL;
    char* headers = NULL;
    char* body = NULL;
    char* method = NULL, *url = NULL;

    if (!SplitRequest(client, &requestLine, &headers, &body) || !SplitRequestLine(requestLine, &method, &url)) {
        send((*client).clientfd, httpErrors[1], strlen(httpErrors[1]), 0);
        pthread_mutex_lock(&stats_mutex);
        errorBytes += strlen(httpErrors[1]);
        errors++;
        pthread_mutex_unlock(&stats_mutex);
        return;
    }

    if (!strcmp("GET", method)) {
        Get(client, url, headers, body);
    } else if (!strcmp("POST", method)) {
        Post(client, url, headers, body);
    } else {
        send((*client).clientfd, httpErrors[1], strlen(httpErrors[1]), 0);
        pthread_mutex_lock(&stats_mutex);
        errorBytes += strlen(httpErrors[1]);
        errors++;
        pthread_mutex_unlock(&stats_mutex);
    }
}