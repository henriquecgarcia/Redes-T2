#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include "rdt.h"

// Configurações da janela e timeout.
#define WINDOW_SIZE   5
#define TIMEOUT_SEC   5
#define TIMEOUT_USEC  1

// Variáveis globais para sequência.
int biterror_inject = FALSE;
hseq_t _snd_seqnum = 1;
hseq_t _rcv_seqnum = 1;

// Função de checksum: calcula a soma de verificação do buffer.
unsigned short checksum(unsigned short *buf, int nbytes) {
    long sum = 0; // Soma de 32 bits
    while (nbytes > 1) { // Processa os bytes em pares (16 bits)
        sum += *buf++; // Soma os 16 bits
        nbytes -= 2; // Avança para o próximo par
    }
    if (nbytes == 1) { // Se houver um byte restante
        sum += *(unsigned char *)buf; // Converte para 16 bits e soma
    }
    while (sum >> 16) // Se houver carry, adiciona ao somatório
        sum = (sum & 0xffff) + (sum >> 16);
    return (unsigned short)(~sum); // Retorna o complemento de 1 da soma
}

// Verifica se o pacote está corrompido.
int iscorrupted(pkt *pr) { // Recebe um ponteiro para o pacote
    pkt copy = *pr; // Cria uma cópia do pacote
    unsigned short recv_csum = copy.h.csum; // Salva o checksum recebido
    copy.h.csum = 0; // Zera o campo de checksum da cópia
    unsigned short calc_csum = checksum((unsigned short *)&copy, copy.h.pkt_size); // Calcula o checksum da cópia
    return (recv_csum != calc_csum); // Retorna TRUE se os checksums forem diferentes
}

// Cria um pacote com o header, cópia do payload (se houver) e cálculo do checksum.
int make_pkt(pkt *p, htype_t type, hseq_t seqnum, void *msg, int msg_len) { // Recebe um ponteiro para o pacote, tipo, sequência, mensagem e tamanho da mensagem
    if (msg_len > MAX_MSG_LEN) { // Se o tamanho da mensagem exceder o limite
        fprintf(stderr, "make_pkt: tamanho da mensagem %d excede MAX_MSG_LEN %d\n", msg_len, MAX_MSG_LEN); // Exibe um erro
        return ERROR;
    }
    p->h.pkt_size = sizeof(hdr); // Inicializa o tamanho do pacote com o tamanho do cabeçalho
    p->h.csum = 0; // Zera o checksum
    p->h.pkt_type = type; // Define o tipo
    p->h.pkt_seq = seqnum; // Define a sequência
    if (msg != NULL && msg_len > 0) { // Se houver mensagem
        p->h.pkt_size += msg_len; // Atualiza o tamanho do pacote
        memset(p->msg, 0, MAX_MSG_LEN); // Zera o buffer de mensagem
        memcpy(p->msg, msg, msg_len); // Copia a mensagem para o buffer
    }
    p->h.csum = checksum((unsigned short *)p, p->h.pkt_size); // Calcula o checksum do pacote
    return SUCCESS; // Retorna sucesso
}

// Verifica se o pacote ACK recebido possui o número de sequência esperado.
int has_ackseq(pkt *p, hseq_t seqnum) { // Recebe um ponteiro para o pacote e o número de sequência esperado
    if (p->h.pkt_type != PKT_ACK || p->h.pkt_seq != seqnum) // Se não for um ACK ou o número de sequência for diferente
        return FALSE; // Retorna FALSE
    return TRUE; // Caso contrário, retorna TRUE
}

// Função para envio de um buffer de dados. Segmenta o buffer em pacotes.
int rdt_send(int sockfd, void *buf, int buf_len, struct sockaddr_in *dst) { // Recebe o socket, buffer, tamanho do buffer e endereço de destino
    int chunk_size = MAX_MSG_LEN; // Tamanho do payload
    int num_segments = (buf_len + chunk_size - 1) / chunk_size; // Número de segmentos necessários
    
    pkt *packets = malloc(num_segments * sizeof(pkt)); // Aloca memória para os pacotes
    if (!packets) { // Se a alocação falhar
        perror("rdt_send: malloc"); // Exibe um erro
        return ERROR;
    }
    
    // Criação dos pacotes (a injeção de erro será aplicada na cópia temporária na hora do envio)
    for (int i = 0; i < num_segments; i++) { // Para cada segmento
        int offset = i * chunk_size; // Calcula o deslocamento
        int remaining = buf_len - offset; // Calcula o tamanho restante
        int seg_len = (remaining > chunk_size) ? chunk_size : remaining; // Calcula o tamanho do segmento
        if (make_pkt(&packets[i], PKT_DATA, _snd_seqnum + i, (char *)buf + offset, seg_len) < 0) { // Cria o pacote
            free(packets); // Libera a memória
            return ERROR;
        }
    }
    
    int base = 0; // Inicializa a base
    int next_seq = 0; // Inicializa a próxima sequência
    struct timeval timeout; // Estrutura para timeout
    fd_set readfds; // Conjunto de descritores de arquivo
    int ns, nr; // Número de bytes enviados e recebidos
    struct sockaddr_in ack_addr; // Endereço do ACK
    socklen_t addrlen; // Tamanho do endereço

    // Variáveis para fast retransmission
    hseq_t last_ack_seq = 0; // Número de sequência do último ACK
    int dup_ack_count = 0; // Contador de ACKs duplicados
    hseq_t fastRetransmittedSeq = 0; // Para registrar o pacote já retransmitido

    while (base < num_segments) { // Enquanto a base for menor que o número de segmentos
        while (next_seq < num_segments && next_seq < base + WINDOW_SIZE) { // Enquanto houver espaço na janela
            
            pkt temp_pkt; // Pacote temporário
            memcpy(&temp_pkt, &packets[next_seq], sizeof(pkt)); // Copia o pacote original
            
            // Injeção de erro (aplicada de forma randômica)
            if (biterror_inject) {
                if (rand() % 100 < 20) {  // 20% de chance
                    printf("rdt_send: Injetando erro no pacote seq %d (tentativa)\n", temp_pkt.h.pkt_seq); // Exibe a injeção
                    memset(temp_pkt.msg, 0, MAX_MSG_LEN); // Zera o payload
                    temp_pkt.h.csum = checksum((unsigned short *)&temp_pkt, temp_pkt.h.pkt_size); // Recalcula o checksum
                }
            }
            
            ns = sendto(sockfd, &temp_pkt, temp_pkt.h.pkt_size, 0, 
                        (struct sockaddr *)dst, sizeof(struct sockaddr_in)); // Envia o pacote
            if (ns < 0) { // Se houver erro
                perror("rdt_send: sendto(PKT_DATA)"); // Exibe o erro
                free(packets); // Libera a memória
                return ERROR;
            }
            printf("rdt_send: Pacote enviado, seq %d\n", packets[next_seq].h.pkt_seq); // Exibe o envio
            next_seq++; // Avança para o próximo pacote
        }
        
        FD_ZERO(&readfds); // Limpa o conjunto de descritores
        FD_SET(sockfd, &readfds); // Adiciona o socket ao conjunto
        timeout.tv_sec = TIMEOUT_SEC; // Configura o timeout em segundos
        timeout.tv_usec = TIMEOUT_USEC; // Configura o timeout em microssegundos
        
        int rv = select(sockfd + 1, &readfds, NULL, NULL, &timeout); // Aguarda a chegada de dados
        if (rv < 0) { // Se houver erro
            perror("rdt_send: select error"); // Exibe o erro
            free(packets);
            return ERROR;
        } else if (rv == 0) { // Se houver timeout
            printf("rdt_send: Timeout. Retransmitindo a partir do pacote seq %d\n", packets[base].h.pkt_seq); // Exibe a retransmissão
            next_seq = base; // Volta para o pacote base
            continue;
        } else { // Se houver dados para leitura
            pkt ack; // Pacote ACK
            addrlen = sizeof(struct sockaddr_in); // Tamanho do endereço
            nr = recvfrom(sockfd, &ack, sizeof(pkt), 0, 
                          (struct sockaddr *)&ack_addr, &addrlen); // Recebe o ACK
            if (nr < 0) { // Se houver erro
                perror("rdt_send: recvfrom(PKT_ACK)"); // Exibe o erro
                free(packets);
                return ERROR;
            }
            if (iscorrupted(&ack) || ack.h.pkt_type != PKT_ACK) { // Se o ACK estiver corrompido ou for inválido
                printf("rdt_send: ACK corrompido ou inválido recebido.\n"); // Exibe o erro
                continue;
            }
            
            // Processamento do ACK com detecção de duplicatas e controle de fast retransmit
            if (ack.h.pkt_seq == last_ack_seq) { // Se o ACK for duplicado
                if (fastRetransmittedSeq != ack.h.pkt_seq) { // Se o ACK duplicado não for para um pacote já retransmitido
                    dup_ack_count++; // Incrementa o contador de ACKs duplicados
                    printf("rdt_send: ACK duplicado (%d) para o pacote seq %d\n", dup_ack_count, ack.h.pkt_seq); // Exibe o ACK duplicado
                    if (dup_ack_count >= 3) { // Se houver 3 ACKs duplicados
                        printf("rdt_send: Fast retransmission disparada para o pacote seq %d\n", packets[base].h.pkt_seq); // Exibe a retransmissão rápida
                        next_seq = base; // Reenvia a partir do pacote faltante
                        fastRetransmittedSeq = ack.h.pkt_seq;  // Marca este pacote como retransmitido
                        dup_ack_count = 0; // Reinicia o contador
                        continue;
                    }
                } else { // Se o ACK duplicado for para um pacote já retransmitido
                    printf("rdt_send: ACK duplicado para o mesmo pacote (seq %d) já retransmitido, ignorando.\n", ack.h.pkt_seq); // Exibe a duplicata
                }
            } else if (ack.h.pkt_seq > last_ack_seq) { // Se o ACK for válido
                last_ack_seq = ack.h.pkt_seq; // Atualiza o número de sequência do último ACK
                dup_ack_count = 0; // Reinicia o contador de ACKs duplicados
                fastRetransmittedSeq = 0;  // Reinicia o marcador
                int ack_index = ack.h.pkt_seq - _snd_seqnum; // Calcula o índice do ACK
                if (ack_index >= base && ack_index < num_segments) { // Se o ACK estiver dentro da janela
                    printf("rdt_send: ACK recebido para o pacote seq %d\n", ack.h.pkt_seq); // Exibe o ACK
                    base = ack_index + 1; // Atualiza a base
                }
            }
        }
    }
    _snd_seqnum += num_segments; // Atualiza a sequência
    free(packets); // Libera a memória
    return buf_len; // Retorna o tamanho do buffer
}

// Função para recepção de um único segmento (usada para mensagens).
int rdt_recv(int sockfd, void *buf, int buf_len, struct sockaddr_in *src) { // Recebe o socket, buffer, tamanho do buffer e endereço de origem
    pkt p, ack; // Pacotes de dados e ACK
    int nr, ns; // Número de bytes recebidos e enviados
    socklen_t addrlen; // Tamanho do endereço
    
    if (make_pkt(&ack, PKT_ACK, _rcv_seqnum - 1, NULL, 0) < 0) // Cria um pacote ACK
        return ERROR; 
    
rerecv: // Rótulo para reenvio
    addrlen = sizeof(struct sockaddr_in); // Tamanho do endereço
    nr = recvfrom(sockfd, &p, sizeof(pkt), 0, (struct sockaddr *)src, &addrlen); // Recebe o pacote
    if (nr < 0) { // Se houver erro
        perror("rdt_recv: recvfrom()"); // Exibe o erro
        return ERROR;
    }
    if (iscorrupted(&p) || p.h.pkt_seq != _rcv_seqnum || p.h.pkt_type != PKT_DATA) { // Se o pacote estiver corrompido ou fora de ordem
        printf("rdt_recv: Pacote corrompido ou fora de ordem (esperado seq %d).\n", _rcv_seqnum); // Exibe o erro
        ns = sendto(sockfd, &ack, ack.h.pkt_size, 0, (struct sockaddr *)src, sizeof(struct sockaddr_in)); // Envia o ACK
        if (ns < 0) { // Se houver erro
            perror("rdt_recv: sendto(PKT_ACK)"); // Exibe o erro
            return ERROR;
        }
        goto rerecv; // Retorna ao rótulo de reenvio
    }
    
    int msg_size = p.h.pkt_size - sizeof(hdr); // Tamanho da mensagem
    if (msg_size > buf_len) { // Se o buffer for insuficiente
        printf("rdt_recv: Buffer insuficiente (%d) para payload (%d).\n", buf_len, msg_size); // Exibe o erro
        return ERROR;
    }
    memcpy(buf, p.msg, msg_size); // Copia a mensagem para o buffer
    
    if (make_pkt(&ack, PKT_ACK, p.h.pkt_seq, NULL, 0) < 0) // Cria um pacote ACK
        return ERROR; 
    ns = sendto(sockfd, &ack, ack.h.pkt_size, 0, (struct sockaddr *)src, sizeof(struct sockaddr_in)); // Envia o ACK
    if (ns < 0) { // Se houver erro
        perror("rdt_recv: sendto(PKT_ACK)"); // Exibe o erro
        return ERROR;
    }
    printf("rdt_recv: Pacote recebido, seq %d. ACK enviado.\n", p.h.pkt_seq); // Exibe a recepção
    _rcv_seqnum++; // Atualiza a sequência
    return msg_size; // Retorna o tamanho da mensagem
}

// Função para enviar um arquivo inteiro. Para arquivos grandes, adapte para leitura em blocos.
int rdt_send_file(int sockfd, const char *filename, struct sockaddr_in *dst) { // Recebe o socket, nome do arquivo e endereço de destino
    FILE *fp = fopen(filename, "rb"); // Abre o arquivo
    if (!fp) { // Se houver erro
        perror("rdt_send_file: fopen"); // Exibe o erro
        return ERROR;
    }
    
    // Obtém o tamanho do arquivo.
    fseek(fp, 0, SEEK_END); // Vai para o final do arquivo
    long fileSize = ftell(fp); // Obtém o tamanho
    rewind(fp); // Retorna ao início
    
    // Aloca um buffer para o arquivo.
    char *fileBuffer = malloc(fileSize); // Aloca memória para o arquivo
    if (!fileBuffer) { // Se a alocação falhar
        perror("rdt_send_file: malloc"); // Exibe o erro
        fclose(fp); // Fecha o arquivo
        return ERROR;
    }
    if (fread(fileBuffer, 1, fileSize, fp) != fileSize) { // Lê o arquivo
        perror("rdt_send_file: fread"); // Exibe o erro
        free(fileBuffer); // Libera a memória
        fclose(fp); // Fecha o arquivo
        return ERROR;
    }
    fclose(fp); // Fecha o arquivo
    
    // Envia os dados do arquivo (a função rdt_send segmenta o buffer).
    int sent = rdt_send(sockfd, fileBuffer, fileSize, dst); // Envia o arquivo
    if (sent < 0) { // Se houver erro
        free(fileBuffer); // Libera a memória
        return ERROR;
    }
    free(fileBuffer); // Libera a memória
    
    // Envia um pacote FIN para sinalizar o fim do arquivo.
    pkt finPkt; // Pacote FIN
    if (make_pkt(&finPkt, PKT_FIN, _snd_seqnum, NULL, 0) < 0) // Cria o pacote FIN
        return ERROR;
    
    int ns = sendto(sockfd, &finPkt, finPkt.h.pkt_size, 0,
                    (struct sockaddr *)dst, sizeof(struct sockaddr_in)); // Envia o pacote
    if (ns < 0) { // Se houver erro
        perror("rdt_send_file: sendto(PKT_FIN)"); // Exibe o erro
        return ERROR;
    }
    printf("rdt_send_file: Pacote FIN enviado (seq %d).\n", finPkt.h.pkt_seq); // Exibe o envio
    
    // Aguarda ACK para o FIN enviado.
    pkt ack; // Pacote ACK
    struct timeval timeout = {TIMEOUT_SEC, TIMEOUT_USEC}; // Timeout
    fd_set readfds; // Conjunto de descritores
    FD_ZERO(&readfds); // Limpa o conjunto
    FD_SET(sockfd, &readfds); // Adiciona o socket
    int rv = select(sockfd + 1, &readfds, NULL, NULL, &timeout); // Aguarda a chegada de dados
    if (rv > 0) { // Se houver dados
        socklen_t addrlen = sizeof(struct sockaddr_in); // Tamanho do endereço
        ns = recvfrom(sockfd, &ack, sizeof(pkt), 0, NULL, &addrlen); // Recebe o ACK
        if (ns < 0) { // Se houver erro
            perror("rdt_send_file: recvfrom(PKT_FIN ACK)"); // Exibe o erro
            return ERROR;
        }
        if (ack.h.pkt_type == PKT_ACK && ack.h.pkt_seq == finPkt.h.pkt_seq) // Se for um ACK válido
            printf("rdt_send_file: ACK do FIN recebido.\n"); // Exibe o ACK
    } else {
        printf("rdt_send_file: Timeout aguardando ACK do FIN.\n"); // Exibe o timeout
    }
    
    // Fase 2: Aguarda FIN do servidor e envia ACK para ele.
    FD_ZERO(&readfds); // Limpa o conjunto
    FD_SET(sockfd, &readfds); // Adiciona o socket
    timeout.tv_sec = TIMEOUT_SEC; // Timeout em segundos
    timeout.tv_usec = TIMEOUT_USEC; // Timeout em microssegundos
    rv = select(sockfd + 1, &readfds, NULL, NULL, &timeout); // Aguarda a chegada de dados
    if (rv > 0) { // Se houver dados
        socklen_t addrlen = sizeof(struct sockaddr_in); // Tamanho do endereço
        ns = recvfrom(sockfd, &ack, sizeof(pkt), 0, NULL, &addrlen); // Recebe o pacote
        if (ns < 0) { // Se houver erro
            perror("rdt_send_file: recvfrom(PKT_FIN do servidor)"); // Exibe o erro
            return ERROR;
        }
        if (ack.h.pkt_type == PKT_FIN) { // Se for um pacote FIN
            printf("rdt_send_file: FIN recebido do servidor (seq %d).\n", ack.h.pkt_seq); // Exibe o FIN
            if (make_pkt(&ack, PKT_ACK, ack.h.pkt_seq, NULL, 0) < 0) // Cria um pacote ACK
                return ERROR; 
            ns = sendto(sockfd, &ack, ack.h.pkt_size, 0, 
                        (struct sockaddr *)dst, sizeof(struct sockaddr_in)); // Envia o ACK
            if (ns < 0) { // Se houver erro
                perror("rdt_send_file: sendto(PKT_ACK para FIN do servidor)"); // Exibe o erro
                return ERROR;
            }
            printf("rdt_send_file: ACK enviado para o FIN do servidor.\n"); // Exibe o ACK
        }
    } else { // Se houver timeout
        printf("rdt_send_file: Timeout aguardando FIN do servidor.\n"); // Exibe o timeout
    }
    
    return sent; // Retorna o tamanho do arquivo
}

// Função para receber um arquivo. Os pacotes são escritos em um arquivo de saída.
int rdt_recv_file(int sockfd, const char *filename) { // Recebe o socket e o nome do arquivo
    FILE *fp = fopen(filename, "wb"); // Abre o arquivo
    if (!fp) { // Se houver erro
        perror("rdt_recv_file: fopen"); // Exibe o erro
        return ERROR;
    }
    
    int totalBytes = 0; // Total de bytes recebidos
    pkt p, ack; // Pacotes de dados e ACK
    struct sockaddr_in src; // Endereço de origem
    socklen_t addrlen; // Tamanho do endereço
    fd_set readfds; // Conjunto de descritores
    struct timeval timeout; // Timeout
    int ns, nr, rv; // Número de bytes enviados e recebidos, e retorno do select
    
    while (1) { // Loop infinito
        addrlen = sizeof(struct sockaddr_in); // Tamanho do endereço
        nr = recvfrom(sockfd, &p, sizeof(pkt), 0,
                      (struct sockaddr *)&src, &addrlen); // Recebe o pacote
        if (nr < 0) { // Se houver erro
            perror("rdt_recv_file: recvfrom()"); // Exibe o erro
            fclose(fp); // Fecha o arquivo
            return ERROR;
        }
        
        if (iscorrupted(&p)) { // Se o pacote estiver corrompido
            printf("rdt_recv_file: Pacote corrompido, reenviando último ACK.\n"); // Exibe o erro
            if (make_pkt(&ack, PKT_ACK, _rcv_seqnum - 1, NULL, 0) < 0) { // Cria um pacote ACK para o último ACK
                fclose(fp); // Fecha o arquivo
                return ERROR;
            }
            sendto(sockfd, &ack, ack.h.pkt_size, 0, 
                   (struct sockaddr *)&src, sizeof(struct sockaddr_in)); // Reenvia o ultimo ACK
            continue;
        }
        
        // Se for um pacote FIN, envia ACK, envia FIN próprio e aguarda ACK para seu FIN.
        if (p.h.pkt_type == PKT_FIN) { 
            // Envia ACK para o FIN recebido do cliente.
            if (make_pkt(&ack, PKT_ACK, p.h.pkt_seq, NULL, 0) < 0) { // Cria um pacote ACK
                fclose(fp); // Fecha o arquivo
                return ERROR;
            }
            sendto(sockfd, &ack, ack.h.pkt_size, 0, 
                   (struct sockaddr *)&src, sizeof(struct sockaddr_in)); // Envia o ACK
            printf("rdt_recv_file: FIN recebido do cliente. ACK enviado para FIN.\n"); // Exibe o ACK
            
            // Envia FIN do servidor.
            pkt serverFin; // Pacote FIN do servidor
            if (make_pkt(&serverFin, PKT_FIN, _snd_seqnum, NULL, 0) < 0) { // Cria o pacote FIN
                fclose(fp); // Fecha o arquivo
                return ERROR;
            }
            ns = sendto(sockfd, &serverFin, serverFin.h.pkt_size, 0,
                        (struct sockaddr *)&src, sizeof(struct sockaddr_in)); // Envia o pacote
            if (ns < 0) { // Se houver erro
                perror("rdt_recv_file: sendto(PKT_FIN do servidor)"); // Exibe o erro
                fclose(fp); // Fecha o arquivo
                return ERROR;
            }
            printf("rdt_recv_file: FIN enviado pelo servidor (seq %d).\n", serverFin.h.pkt_seq); // Exibe o envio
            
            // (Opcional) Aguarda ACK para o FIN do servidor.
            FD_ZERO(&readfds); // Limpa o conjunto
            FD_SET(sockfd, &readfds); // Adiciona o socket
            timeout.tv_sec = TIMEOUT_SEC; // Timeout em segundos
            timeout.tv_usec = TIMEOUT_USEC; // Timeout em microssegundos
            rv = select(sockfd + 1, &readfds, NULL, NULL, &timeout); // Aguarda a chegada de dados
            if (rv > 0) { // Se houver dados
                socklen_t ackAddrLen = sizeof(struct sockaddr_in); // Tamanho do endereço
                nr = recvfrom(sockfd, &ack, sizeof(pkt), 0, (struct sockaddr *)&src, &ackAddrLen); // Recebe o ACK
                if (nr > 0 && ack.h.pkt_type == PKT_ACK && ack.h.pkt_seq == serverFin.h.pkt_seq) { // Se for um ACK válido
                    printf("rdt_recv_file: ACK recebido para o FIN do servidor.\n"); // Exibe o ACK
                }
            }
            break; // Encerra o loop
        }
        
        // Se for um pacote de dados com a sequência esperada.
        if (p.h.pkt_type == PKT_DATA && p.h.pkt_seq == _rcv_seqnum) { // Se for um pacote de dados e a sequência for a esperada
            int dataSize = p.h.pkt_size - sizeof(hdr); // Tamanho dos dados
            if (fwrite(p.msg, 1, dataSize, fp) != dataSize) { // Escreve os dados no arquivo
                perror("rdt_recv_file: fwrite"); // Exibe o erro
                fclose(fp); // Fecha o arquivo
                return ERROR;
            }
            totalBytes += dataSize; // Atualiza o total de bytes
            printf("rdt_recv_file: Pacote recebido, seq %d (%d bytes).\n", p.h.pkt_seq, dataSize); // Exibe a recepção
            
            if (make_pkt(&ack, PKT_ACK, p.h.pkt_seq, NULL, 0) < 0) { // Cria um pacote ACK
                fclose(fp); // Fecha o arquivo
                return ERROR;
            }
            sendto(sockfd, &ack, ack.h.pkt_size, 0,
                   (struct sockaddr *)&src, sizeof(struct sockaddr_in)); // Envia o ACK
            _rcv_seqnum++; // Atualiza a sequência
        } else { // Caso o pacote esteja fora de ordem, reenvia o último ACK cumulativo.
            printf("rdt_recv_file: Pacote fora de ordem (esperado seq %d).\n", _rcv_seqnum); // Exibe o erro
            if (make_pkt(&ack, PKT_ACK, _rcv_seqnum - 1, NULL, 0) < 0) { // Cria um pacote ACK
                fclose(fp); // Fecha o arquivo
                return ERROR;
            }
            sendto(sockfd, &ack, ack.h.pkt_size, 0,
                   (struct sockaddr *)&src, sizeof(struct sockaddr_in)); // Envia o ACK
        }
    }
    
    fclose(fp); // Fecha o arquivo
    printf("rdt_recv_file: Transferência concluída. Total de bytes recebidos: %d\n", totalBytes); // Exibe o total de bytes
    return totalBytes; // Retorna o total de bytes
}
