#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "rdt.h"

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <server_ip> <server_port> <arquivo>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    char *server_ip = argv[1];
    int server_port = atoi(argv[2]);
    char *filename = argv[3];
    
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("client: socket");
        exit(EXIT_FAILURE);
    }
    
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &dest_addr.sin_addr) <= 0) {
        perror("client: inet_pton");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    
    printf("client: Enviando arquivo '%s' para %s:%d\n", filename, server_ip, server_port);
    
    if (rdt_send_file(sockfd, filename, &dest_addr) < 0) {
        fprintf(stderr, "client: Erro ao enviar o arquivo.\n");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    
    printf("client: Arquivo enviado com sucesso.\n");
    close(sockfd);
    return 0;
}
