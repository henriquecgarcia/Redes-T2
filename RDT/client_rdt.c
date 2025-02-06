#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "rdt.h"

int main(int argc, char *argv[]) {
    if (argc != 4) { // Se o número de argumentos for diferente de 4
        fprintf(stderr, "Uso: %s <server_ip> <server_port> <arquivo>\n", argv[0]); // Exibe a mensagem de uso
        exit(EXIT_FAILURE);
    }
    
    char *server_ip = argv[1]; // Obtém o IP do servidor
    int server_port = atoi(argv[2]); // Obtém a porta do servidor
    char *filename = argv[3]; // Obtém o nome do arquivo
    
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0); // Cria o socket
    if (sockfd < 0) { // Se houver erro
        perror("client: socket"); // Exibe o erro
        exit(EXIT_FAILURE);
    }
    
    struct sockaddr_in dest_addr; // Endereço de destino
    memset(&dest_addr, 0, sizeof(dest_addr)); // Zera a estrutura
    dest_addr.sin_family = AF_INET; // Define a família
    dest_addr.sin_port = htons(server_port); // Define a porta
    if (inet_pton(AF_INET, server_ip, &dest_addr.sin_addr) <= 0) { // Converte o IP
        perror("client: inet_pton"); // Exibe o erro
        close(sockfd); // Fecha o socket
        exit(EXIT_FAILURE); 
    }
    
    printf("client: Enviando arquivo '%s' para %s:%d\n", filename, server_ip, server_port); // Exibe a mensagem de envio
    if (rdt_send_file(sockfd, filename, &dest_addr) < 0) { // Envia o arquivo
        fprintf(stderr, "client: Erro ao enviar o arquivo.\n"); // Exibe o erro
        close(sockfd); // Fecha o socket
        exit(EXIT_FAILURE);
    }
    
    printf("client: Arquivo enviado com sucesso.\n"); // Exibe a mensagem de sucesso
    close(sockfd); // Fecha o socket
    return 0;
}
