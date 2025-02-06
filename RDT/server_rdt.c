#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "rdt.h"

int main(int argc, char *argv[]) {
    if (argc != 3) { // Se o número de argumentos for diferente de 3
        fprintf(stderr, "Uso: %s <porta_de_escuta> <arquivo_saida>\n", argv[0]); // Exibe a mensagem de uso
        exit(EXIT_FAILURE); // Encerra o programa
    }
    
    int listen_port = atoi(argv[1]); // Obtém a porta de escuta
    char *output_filename = argv[2]; // Obtém o nome do arquivo de saída
    
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0); // Cria o socket
    if (sockfd < 0) { // Se houver erro
        perror("server: socket"); // Exibe o erro
        exit(EXIT_FAILURE);
    }
    
    struct sockaddr_in server_addr; // Endereço do servidor
    memset(&server_addr, 0, sizeof(server_addr)); // Zera a estrutura
    server_addr.sin_family = AF_INET; // Define a família
    server_addr.sin_port = htons(listen_port); // Define a porta
    server_addr.sin_addr.s_addr = INADDR_ANY; // Define o endereço
    
    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) { // Associa o socket à porta
        perror("server: bind"); // Exibe o erro
        close(sockfd); // Fecha o socket
        exit(EXIT_FAILURE);
    }
    
    printf("server: Escutando na porta %d. Salvando arquivo em '%s'\n", listen_port, output_filename); // Exibe a mensagem de escuta
    if (rdt_recv_file(sockfd, output_filename) < 0) { // Recebe o arquivo
        fprintf(stderr, "server: Erro ao receber o arquivo.\n"); // Exibe o erro
        close(sockfd); // Fecha o socket
        exit(EXIT_FAILURE);
    }
    
    printf("server: Arquivo recebido com sucesso.\n"); // Exibe a mensagem de sucesso
    close(sockfd); // Fecha o socket
    return 0;
}
