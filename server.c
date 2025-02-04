#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include "rdt.h"

#define PORT 8080
#define BUFFER_SIZE 1024

int main() {
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    char buffer[BUFFER_SIZE];

    // Criar socket UDP
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Falha ao criar socket");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    // Vincular o socket
    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Falha ao vincular");
        exit(EXIT_FAILURE);
    }

    printf("Servidor RDT iniciado na porta %d...\n", PORT);

    while (1) {
        int recv_len = rdt_recv(sockfd, buffer, BUFFER_SIZE, &client_addr);

        if (recv_len < 0) {
            printf("rdt_recv: erro ao receber pacote.\n");
            continue;
        }

        buffer[recv_len] = '\0';
        printf("Mensagem recebida (seqnum esperado): %s\n", buffer);

        // Enviar ACK corretamente
        char response[BUFFER_SIZE] = "Mensagem recebida com sucesso";
        
        if (rdt_send(sockfd, response, strlen(response), &client_addr) < 0) {
            perror("rdt_send falhou");
        }
    }

    close(sockfd);
    return 0;
}
