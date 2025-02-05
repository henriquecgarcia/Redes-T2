#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "rdt.h"

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <porta_de_escuta> <arquivo_saida>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    int listen_port = atoi(argv[1]);
    char *output_filename = argv[2];
    
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("server: socket");
        exit(EXIT_FAILURE);
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(listen_port);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("server: bind");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    
    printf("server: Escutando na porta %d. Salvando arquivo em '%s'\n", listen_port, output_filename);
    if (rdt_recv_file(sockfd, output_filename) < 0) {
        fprintf(stderr, "server: Erro ao receber o arquivo.\n");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    
    printf("server: Arquivo recebido com sucesso.\n");
    close(sockfd);
    return 0;
}
