#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "rdt.h"

#define MAX_DATA_SIZE 65536  // 64k

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
    
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("client: fopen");
        return ERROR;
    }

    char buffer[MAX_DATA_SIZE];
    size_t bytesRead;
    
    fseek(fp, 0, SEEK_END);
    long fileSize = ftell(fp);
    rewind(fp);
    
    // Envia o PKT_START com os metadados do arquivo usando seq 0.
    file_meta meta;
    memset(&meta, 0, sizeof(file_meta));
    strncpy(meta.filename, filename, sizeof(meta.filename)-1);
    meta.fileSize = fileSize;
    
    pkt startPkt;
    
    if (make_pkt(&startPkt, PKT_START, 0, &meta, sizeof(file_meta)) < 0) {
        fclose(fp);
        return ERROR;
    }
    int ns = sendto(sockfd, &startPkt, startPkt.h.pkt_size, 0,
                    (struct sockaddr *)&dest_addr, sizeof(struct sockaddr_in));
    if (ns < 0) {
        perror("client: sendto(PKT_START)");
        fclose(fp);
        return ERROR;
    }

    printf("client: PKT_START enviado (seq %d). Nome: %s, Tamanho: %ld bytes.\n", 
           startPkt.h.pkt_seq, meta.filename, meta.fileSize);

    // Loop para ler o arquivo em blocos de 64k
    while ((bytesRead = fread(buffer, 1, MAX_DATA_SIZE, fp)) > 0) {
        if(rdt_send(sockfd, buffer, bytesRead, &dest_addr) < 0) {
            perror("client: rdt_send");
            fclose(fp);
            return ERROR;
        }
    }

    if(rdt_close(sockfd, &dest_addr, 0) < 0) {
        perror("client: rdt_close");
        fclose(fp);
        return ERROR;
    }
    
    printf("client: Arquivo enviado com sucesso.\n");
    close(sockfd);
    return 0;
}
