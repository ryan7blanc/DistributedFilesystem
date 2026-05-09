#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>
#include <ctype.h>
#include <errno.h>
#include <openssl/md5.h>
#include <dirent.h>
#include <time.h>

#define BUFSIZE 4096
#define CACHE_DIR "./cache"
#define MAX_CONNECT 100
#define HTTP "http://"
#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)


int proxy_running = 1;
int proxy_sock;
int timeout;

void sigint_handler(int sig) {
   printf("\nShutting down proxy...\n");
   proxy_running = 0;
   close(proxy_sock);
   exit(0);
}
void error_handler(int client_sock, const char *status_code) {
   char response[1024];
   snprintf(response, sizeof(response), "HTTP/1.1 %s\r\nContent-Type: N/A\r\nContent-Length: 0\r\n\r\n", status_code);
   send(client_sock, response, strlen(response), 0);
}

void md5(const char *url, char *output) {
   unsigned char hash[MD5_DIGEST_LENGTH];
   const unsigned char *data = (const unsigned char *)url;
   size_t len = strlen(url);

   MD5_CTX ctx;
   MD5_Init(&ctx);
   MD5_Update(&ctx, data, len);
   MD5_Final(hash, &ctx);

   char *ptr = output;
   for (int i = 0; i < MD5_DIGEST_LENGTH; ++i) {
       snprintf(ptr, 3, "%02x", hash[i]);
       ptr += 2;
   }
}

int t_direc(const char *hash, char *filename) {
   //traverse cache directory
   snprintf(filename, 512, "%s/%s", CACHE_DIR, hash);
   struct stat st;
   if (!stat(filename, &st))
   {
       return 0;
   }
   else
   {
       time_t nowtime = time(NULL);
       if ((nowtime - st.st_mtime) < timeout)
       {
           return 1;
       }
   }
   return 0;
}

int cache_sender(const char *hash, int client_sock, char * filename) {
   char output[BUFSIZE];
   ssize_t reading;
   FILE *reader = fopen(filename, "rb");
   if (!reader)
   {
       printf("file %s can not be opened\n", filename);
       return 1;
   }

   while ((reading = fread(output, 1, sizeof(output), reader)) > 0) {
       send(client_sock, output, reading, 0);
   }
   fclose(reader);
   return 0;
}
void store_cache(char *filename, char *data, size_t lent)
{
   FILE *writer = fopen(filename, "wb");
   if (!writer)
   {
       printf("can not write file %s\n", filename);
       return;
   }
   fwrite(data, 1, lent, writer);
   fclose(writer);
}

int check_request(char *request, char *host, int *port, char *path, int client_sock) {
   char method[16], url[1024], version[16];

   if (sscanf(request, "%"STRINGIFY(15)"s %"STRINGIFY(1023)"s %"STRINGIFY(15)"s", method, url, version) != 3 || strcmp(method, "GET") != 0) {
       error_handler(client_sock, "400 Bad Request");
       return 1;
   }

   char *start = strstr(url, HTTP);
   start = start ? start + 7 : url;

   char *slash = strchr(start, '/');
   if (slash) {
       strncpy(host, start, slash - start);
       host[slash - start] = '\0';
       strcpy(path, slash);
   }
   else {
       strcpy(host, start);
       strcpy(path, "/");
   }

   char *colon = strchr(host, ':');
   if (colon) {
       *colon = '\0';
       *port = atoi(colon + 1);
   }
   else {
       *port = 80;
   }

   return 0;
}

void server_request(char *host, int port, char *path, char *filename, int client_sock) {
   struct addrinfo hints = {0}, *res, *p;
   char port_str[6], buffer[BUFSIZE], request[BUFSIZE];
   int server_sock = -1;
   ssize_t bytes_read;

   snprintf(port_str, sizeof(port_str), "%d", port);
   hints.ai_family = AF_INET;
   hints.ai_socktype = SOCK_STREAM;

   if (getaddrinfo(host, port_str, &hints, &res) != 0) {
       perror("getaddrinfo");
       error_handler(client_sock, "404 Not Found");
       return;
   }

   for (p = res; p; p = p->ai_next) {
       server_sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
       if (server_sock >= 0 && connect(server_sock, p->ai_addr, p->ai_addrlen) == 0)
           break;
       if (server_sock >= 0) close(server_sock);
   }
   free(res);

   if (!p) {
       perror("connect");
       error_handler(client_sock, "502 Bad Gateway");
       return;
   }

   snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: %s\r\n\r\n", path, host);
   if (send(server_sock, request, strlen(request), 0) < 0) {
       perror("send");
       error_handler(client_sock, "500 Internal Server Error");
       close(server_sock);
       return;
   }

   FILE *cache_file = fopen(filename, "wb");
   if (!cache_file) perror("fopen");

   while ((bytes_read = recv(server_sock, buffer, sizeof(buffer), 0)) > 0) {
       send(client_sock, buffer, bytes_read, 0);
       if (cache_file) fwrite(buffer, 1, bytes_read, cache_file);
   }

   if (cache_file) fclose(cache_file);
   close(server_sock);
}

void *connection_handler(void* client_sock) {
   char buf[BUFSIZE], method[16], url[1024], version[16];
   char hostname[256], path[1024];
   char newfilename[512];
   char url_hash[MD5_DIGEST_LENGTH * 2 + 1];
   int port;

   int client_sock2 = *((int *)client_sock);
   free(client_sock);

       ssize_t recvsize = recv(client_sock2, buf, sizeof(buf) - 1, 0);
       if (recvsize <= 0)
       {
           close(client_sock2);
           return NULL;
       }
       buf[recvsize] = '\0';
       check_request(buf, hostname, &port, path, client_sock2);

       md5(buf, url_hash);
       snprintf(url, sizeof(url), "%s/%s", CACHE_DIR, url_hash);
       if (t_direc(url_hash, url)) {
          int result = cache_sender(url_hash, client_sock2, url_hash);
          if (result == 0)
          {
           perror("send cache failed\n");
          }
       }
       else
       {
          server_request(hostname, port, path, url, client_sock2);
       }
       close(client_sock2);

}


int main(int argc, char *argv[]) {
   if (argc != 3) {
       return 1;
   }
   int c, *new_sock, optval;

   int port = atoi(argv[1]);
   int timeout = atoi(argv[2]);

   struct sigaction exit;
   exit.sa_handler = sigint_handler;
   sigaction(SIGINT, &exit, NULL);

   proxy_sock = socket(AF_INET, SOCK_STREAM, 0);
   if (proxy_sock == -1) {
       printf("Could not create socket\n");
       return 1;
   }
   puts("Proxy socket created");

   struct sockaddr_in proxy_addr;
   proxy_addr.sin_family = AF_INET;
   proxy_addr.sin_addr.s_addr = INADDR_ANY;
   proxy_addr.sin_port = htons(port);

   if (bind(proxy_sock, (struct sockaddr *)&proxy_addr, sizeof(proxy_addr)) < 0) {
       perror("Bind failed");
       return 1;
   }
   puts("Bind done");

   if (listen(proxy_sock, MAX_CONNECT) < 0) {
       puts("Listen failed");
       return 1;
   }
   puts("Server is listening...");
   mkdir(CACHE_DIR, 0777);

   while (proxy_running) {
       struct sockaddr_in client_addr;
       socklen_t client_len = sizeof(client_addr);
       int *client_sock = malloc(sizeof(int));

       *client_sock = accept(proxy_sock, (struct sockaddr *)&client_addr, &client_len);
       if (*client_sock < 0) {
       free(client_sock);
       perror("Accept failed");
       continue;
       }

       puts("Connection accepted");

       pthread_t thread;
       if (pthread_create(&thread, NULL, connection_handler, client_sock) != 0) {
           free(client_sock);
           perror("Thread creation failed");
           continue;
       }
       pthread_detach(thread);
   }

   close(proxy_sock);
   return 0;
}
