#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "rdt.h"

// Configurações de janela de transmissão
#define STATIC_WINDOW_SIZE 5
#define MAX_DYNAMIC_WINDOW 20
#define MIN_DYNAMIC_WINDOW 1

// Flag para escolher o modo de janela: 0 = estática, 1 = dinâmica.
int dynamic_window_enabled = 1;
int current_window_size = STATIC_WINDOW_SIZE;

// Timeout
#define TIMEOUT_SEC 5
#define TIMEOUT_USEC 1

int main(int argc, char *argv[]) {
    if (argc != 2) { 
        fprintf(stderr, "Uso: %s <porta_de_escuta>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    int listen_port = atoi(argv[1]);
    
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
    
    printf("server: Escutando na porta %d.\n", listen_port);
    
    /* 
     * Note que o rdt_recv_file() agora espera receber um pacote PKT_START com os
     * metadados do arquivo. O nome do arquivo a ser criado será aquele enviado pelo cliente.
     * O parâmetro 'output_filename' é utilizado como fallback (se não houver metadados válidos).
     */
    if (rdt_recv_file(sockfd, "output.bin") < 0) {
        fprintf(stderr, "server: Erro ao receber o arquivo.\n");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    
    printf("server: Arquivo recebido com sucesso.\n");
    close(sockfd);
    return 0;
}
