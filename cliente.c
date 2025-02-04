#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "rdt.h"

#define BUFFER_SIZE 1024

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <endereço_ip> <porta>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int sockfd;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];

    // Criar socket UDP
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Falha ao criar socket");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(atoi(argv[2]));
    
    if (inet_pton(AF_INET, argv[1], &server_addr.sin_addr) <= 0) {
        perror("Endereço inválido / Endereço não suportado");
        exit(EXIT_FAILURE);
    }

    printf("Conectado ao servidor %s:%s\n", argv[1], argv[2]);

    while (1) {
        printf("Digite uma mensagem (ou 'sair' para encerrar): ");
        fgets(buffer, BUFFER_SIZE, stdin);
        buffer[strcspn(buffer, "\n")] = 0;

        if (strcmp(buffer, "sair") == 0) {
            break;
        }

        int retries = 0;
        int max_retries = 5;
        int timeout = 1000; // 1 segundo inicial

        while (retries < max_retries) {
            printf("Enviando mensagem (tentativa %d)...\n", retries + 1);
            if (rdt_send(sockfd, buffer, strlen(buffer), &server_addr) < 0) {
                perror("rdt_send falhou");
                usleep(timeout * 1000);
                timeout *= 2; // Backoff exponencial
                retries++;
            } else {
                break;
            }
        }

        if (retries == max_retries) {
            printf("Falha ao enviar após %d tentativas\n", max_retries);
            continue;
        }

        // Receber resposta
        char response[BUFFER_SIZE];
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        int recv_len = rdt_recv(sockfd, response, BUFFER_SIZE, &from_addr);
        
        if (recv_len < 0) {
            perror("rdt_recv falhou");
            continue;
        }
        
        response[recv_len] = '\0';
        printf("Resposta do servidor: %s\n", response);
    }

    close(sockfd);
    return 0;
}
