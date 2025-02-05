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
#define TIMEOUT_SEC   10
#define TIMEOUT_USEC  1

// Variáveis globais para sequência.
int biterror_inject = TRUE;
hseq_t _snd_seqnum = 1;
hseq_t _rcv_seqnum = 1;

// Função de checksum: calcula a soma de verificação do buffer.
unsigned short checksum(unsigned short *buf, int nbytes) {
    long sum = 0;
    while (nbytes > 1) {
        sum += *buf++;
        nbytes -= 2;
    }
    if (nbytes == 1) {
        sum += *(unsigned char *)buf;
    }
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (unsigned short)(~sum);
}

// Verifica se o pacote está corrompido.
int iscorrupted(pkt *pr) {
    pkt copy = *pr;
    unsigned short recv_csum = copy.h.csum;
    copy.h.csum = 0;
    unsigned short calc_csum = checksum((unsigned short *)&copy, copy.h.pkt_size);
    return (recv_csum != calc_csum);
}

// Cria um pacote com o header, cópia do payload (se houver) e cálculo do checksum.
int make_pkt(pkt *p, htype_t type, hseq_t seqnum, void *msg, int msg_len) {
    if (msg_len > MAX_MSG_LEN) {
        fprintf(stderr, "make_pkt: tamanho da mensagem %d excede MAX_MSG_LEN %d\n", msg_len, MAX_MSG_LEN);
        return ERROR;
    }
    p->h.pkt_size = sizeof(hdr);
    p->h.csum = 0;
    p->h.pkt_type = type;
    p->h.pkt_seq = seqnum;
    if (msg != NULL && msg_len > 0) {
        p->h.pkt_size += msg_len;
        memset(p->msg, 0, MAX_MSG_LEN);
        memcpy(p->msg, msg, msg_len);
    }
    p->h.csum = checksum((unsigned short *)p, p->h.pkt_size);
    return SUCCESS;
}

// Verifica se o pacote ACK recebido possui o número de sequência esperado.
int has_ackseq(pkt *p, hseq_t seqnum) {
    if (p->h.pkt_type != PKT_ACK || p->h.pkt_seq != seqnum)
        return FALSE;
    return TRUE;
}

int rdt_send(int sockfd, void *buf, int buf_len, struct sockaddr_in *dst) {
    int chunk_size = MAX_MSG_LEN;
    int num_segments = (buf_len + chunk_size - 1) / chunk_size;
    
    pkt *packets = malloc(num_segments * sizeof(pkt));
    if (!packets) {
        perror("rdt_send: malloc");
        return ERROR;
    }
    
    // Criação dos pacotes (sem injeção de erro aqui, a injeção será aplicada na cópia temporária na hora do envio)
    for (int i = 0; i < num_segments; i++) {
        int offset = i * chunk_size;
        int remaining = buf_len - offset;
        int seg_len = (remaining > chunk_size) ? chunk_size : remaining;
        if (make_pkt(&packets[i], PKT_DATA, _snd_seqnum + i, (char *)buf + offset, seg_len) < 0) {
            free(packets);
            return ERROR;
        }
    }
    
    int base = 0;
    int next_seq = 0;
    struct timeval timeout;
    fd_set readfds;
    int ns, nr;
    struct sockaddr_in ack_addr;
    socklen_t addrlen;

    // Variáveis para fast retransmission
    hseq_t last_ack_seq = 0;
    int dup_ack_count = 0;
    hseq_t fastRetransmittedSeq = 0;  // Variável para registrar o pacote já retransmitido

    
    while (base < num_segments) {
        while (next_seq < num_segments && next_seq < base + WINDOW_SIZE) {
            // Cria uma cópia temporária do pacote original
            pkt temp_pkt;
            memcpy(&temp_pkt, &packets[next_seq], sizeof(pkt));
            
            // Injeção de erro (como já implementado anteriormente)
            if (biterror_inject) {
                if (rand() % 100 < 20) {  // 20% de chance
                    printf("rdt_send: Injetando erro no pacote seq %d (tentativa)\n", temp_pkt.h.pkt_seq);
                    memset(temp_pkt.msg, 0, MAX_MSG_LEN);
                    temp_pkt.h.csum = checksum((unsigned short *)&temp_pkt, temp_pkt.h.pkt_size);
                }
            }
            
            ns = sendto(sockfd, &temp_pkt, temp_pkt.h.pkt_size, 0,
                        (struct sockaddr *)dst, sizeof(struct sockaddr_in));
            if (ns < 0) {
                perror("rdt_send: sendto(PKT_DATA)");
                free(packets);
                return ERROR;
            }
            printf("rdt_send: Pacote enviado, seq %d\n", packets[next_seq].h.pkt_seq);
            next_seq++;
        }
        
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        timeout.tv_sec = TIMEOUT_SEC;
        timeout.tv_usec = TIMEOUT_USEC;
        
        int rv = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
        if (rv < 0) {
            perror("rdt_send: select error");
            free(packets);
            return ERROR;
        } else if (rv == 0) {
            printf("rdt_send: Timeout. Retransmitindo a partir do pacote seq %d\n", packets[base].h.pkt_seq);
            next_seq = base;
            continue;
        } else {
            pkt ack;
            addrlen = sizeof(struct sockaddr_in);
            nr = recvfrom(sockfd, &ack, sizeof(pkt), 0,
                          (struct sockaddr *)&ack_addr, &addrlen);
            if (nr < 0) {
                perror("rdt_send: recvfrom(PKT_ACK)");
                free(packets);
                return ERROR;
            }
            if (iscorrupted(&ack) || ack.h.pkt_type != PKT_ACK) {
                printf("rdt_send: ACK corrompido ou inválido recebido.\n");
                continue;
            }
            
                        // Processamento do ACK com detecção de duplicatas e controle de fast retransmit
            if (ack.h.pkt_seq == last_ack_seq) {
                // Se o ACK duplicado for para o mesmo pacote que já foi retransmitido, ignore
                if (fastRetransmittedSeq != ack.h.pkt_seq) {
                    dup_ack_count++;
                    printf("rdt_send: ACK duplicado (%d) para o pacote seq %d\n", dup_ack_count, ack.h.pkt_seq);
                    if (dup_ack_count >= 3) {
                        printf("rdt_send: Fast retransmission disparada para o pacote seq %d\n", packets[base].h.pkt_seq);
                        next_seq = base; // Reenvia a partir do pacote faltante
                        fastRetransmittedSeq = ack.h.pkt_seq;  // Marca este pacote como retransmitido
                        dup_ack_count = 0;
                        continue;
                    }
                } else {
                    // Já houve fast retransmit para esse pacote; ignore ACKs duplicados subsequentes.
                    printf("rdt_send: ACK duplicado para o mesmo pacote (seq %d) já retransmitido, ignorando.\n", ack.h.pkt_seq);
                }
            } else if (ack.h.pkt_seq > last_ack_seq) {
                // Chegou um novo ACK que avança a janela
                last_ack_seq = ack.h.pkt_seq;
                dup_ack_count = 0;
                fastRetransmittedSeq = 0;  // Reinicia o marcador, permitindo novo fast retransmit se necessário
                int ack_index = ack.h.pkt_seq - _snd_seqnum;
                if (ack_index >= base && ack_index < num_segments) {
                    printf("rdt_send: ACK recebido para o pacote seq %d\n", ack.h.pkt_seq);
                    base = ack_index + 1;
                }
            }

        }
    }
    _snd_seqnum += num_segments;
    free(packets);
    return buf_len;
}



// Função para recepção de um único segmento (usada para mensagens).
int rdt_recv(int sockfd, void *buf, int buf_len, struct sockaddr_in *src) {
    pkt p, ack;
    int nr, ns;
    socklen_t addrlen;
    
    if (make_pkt(&ack, PKT_ACK, _rcv_seqnum - 1, NULL, 0) < 0)
        return ERROR;
    
rerecv:
    addrlen = sizeof(struct sockaddr_in);
    nr = recvfrom(sockfd, &p, sizeof(pkt), 0, (struct sockaddr *)src, &addrlen);
    if (nr < 0) {
        perror("rdt_recv: recvfrom()");
        return ERROR;
    }
    if (iscorrupted(&p) || p.h.pkt_seq != _rcv_seqnum || p.h.pkt_type != PKT_DATA) {
        printf("rdt_recv: Pacote corrompido ou fora de ordem (esperado seq %d).\n", _rcv_seqnum);
        ns = sendto(sockfd, &ack, ack.h.pkt_size, 0, (struct sockaddr *)src, sizeof(struct sockaddr_in));
        if (ns < 0) {
            perror("rdt_recv: sendto(PKT_ACK)");
            return ERROR;
        }
        goto rerecv;
    }
    
    int msg_size = p.h.pkt_size - sizeof(hdr);
    if (msg_size > buf_len) {
        printf("rdt_recv: Buffer insuficiente (%d) para payload (%d).\n", buf_len, msg_size);
        return ERROR;
    }
    memcpy(buf, p.msg, msg_size);
    
    if (make_pkt(&ack, PKT_ACK, p.h.pkt_seq, NULL, 0) < 0)
        return ERROR;
    ns = sendto(sockfd, &ack, ack.h.pkt_size, 0, (struct sockaddr *)src, sizeof(struct sockaddr_in));
    if (ns < 0) {
        perror("rdt_recv: sendto(PKT_ACK)");
        return ERROR;
    }
    printf("rdt_recv: Pacote recebido, seq %d. ACK enviado.\n", p.h.pkt_seq);
    _rcv_seqnum++;
    return msg_size;
}

// Função para enviar um arquivo inteiro. Neste exemplo, o arquivo é carregado
// completamente na memória. Para arquivos grandes, adapte para ler em blocos.
int rdt_send_file(int sockfd, const char *filename, struct sockaddr_in *dst) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("rdt_send_file: fopen");
        return ERROR;
    }
    
    // Obtém o tamanho do arquivo.
    fseek(fp, 0, SEEK_END);
    long fileSize = ftell(fp);
    rewind(fp);
    
    // Aloca um buffer para o arquivo.
    char *fileBuffer = malloc(fileSize);
    if (!fileBuffer) {
        perror("rdt_send_file: malloc");
        fclose(fp);
        return ERROR;
    }
    if (fread(fileBuffer, 1, fileSize, fp) != fileSize) {
        perror("rdt_send_file: fread");
        free(fileBuffer);
        fclose(fp);
        return ERROR;
    }
    fclose(fp);
    
    // Envia os dados do arquivo (a função rdt_send segmenta o buffer).
    int sent = rdt_send(sockfd, fileBuffer, fileSize, dst);
    if (sent < 0) {
        free(fileBuffer);
        return ERROR;
    }
    free(fileBuffer);
    
    // Envia um pacote FIN para sinalizar o fim do arquivo.
    pkt finPkt;
    if (make_pkt(&finPkt, PKT_FIN, _snd_seqnum, NULL, 0) < 0)
        return ERROR;
    
    int ns = sendto(sockfd, &finPkt, finPkt.h.pkt_size, 0,
                    (struct sockaddr *)dst, sizeof(struct sockaddr_in));
    if (ns < 0) {
        perror("rdt_send_file: sendto(PKT_FIN)");
        return ERROR;
    }
    printf("rdt_send_file: Pacote FIN enviado (seq %d).\n", finPkt.h.pkt_seq);
    
    // (Opcional) Aguarda o ACK do FIN.
    pkt ack;
    struct timeval timeout = {TIMEOUT_SEC, TIMEOUT_USEC};
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    int rv = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
    if (rv > 0) {
        socklen_t addrlen = sizeof(struct sockaddr_in);
        ns = recvfrom(sockfd, &ack, sizeof(pkt), 0, NULL, &addrlen);
        if (ns < 0) {
            perror("rdt_send_file: recvfrom(PKT_FIN ACK)");
            return ERROR;
        }
        if (ack.h.pkt_type == PKT_ACK && ack.h.pkt_seq == finPkt.h.pkt_seq)
            printf("rdt_send_file: ACK do FIN recebido.\n");
    } else {
        printf("rdt_send_file: Timeout aguardando ACK do FIN.\n");
    }
    
    return sent;
}

// Função para receber um arquivo. Os pacotes serão escritos num arquivo de saída.
int rdt_recv_file(int sockfd, const char *filename) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror("rdt_recv_file: fopen");
        return ERROR;
    }
    
    int totalBytes = 0;
    pkt p, ack;
    struct sockaddr_in src;
    socklen_t addrlen;
    
    while (1) {
        addrlen = sizeof(struct sockaddr_in);
        int nr = recvfrom(sockfd, &p, sizeof(pkt), 0,
                          (struct sockaddr *)&src, &addrlen);
        if (nr < 0) {
            perror("rdt_recv_file: recvfrom()");
            fclose(fp);
            return ERROR;
        }
        
        // Se o pacote estiver corrompido, reenvia o último ACK.
        if (iscorrupted(&p)) {
            printf("rdt_recv_file: Pacote corrompido, reenviando último ACK.\n");
            make_pkt(&ack, PKT_ACK, _rcv_seqnum - 1, NULL, 0);
            sendto(sockfd, &ack, ack.h.pkt_size, 0,
                   (struct sockaddr *)&src, sizeof(struct sockaddr_in));
            continue;
        }
        
        // Se for um pacote FIN, envia ACK e encerra a recepção.
        if (p.h.pkt_type == PKT_FIN) {
            make_pkt(&ack, PKT_ACK, p.h.pkt_seq, NULL, 0);
            sendto(sockfd, &ack, ack.h.pkt_size, 0,
                   (struct sockaddr *)&src, sizeof(struct sockaddr_in));
            printf("rdt_recv_file: Pacote FIN recebido. Transferência finalizada.\n");
            break;
        }
        
        // Se for um pacote de dados com a sequência esperada.
        if (p.h.pkt_type == PKT_DATA && p.h.pkt_seq == _rcv_seqnum) {
            int dataSize = p.h.pkt_size - sizeof(hdr);
            if (fwrite(p.msg, 1, dataSize, fp) != dataSize) {
                perror("rdt_recv_file: fwrite");
                fclose(fp);
                return ERROR;
            }
            totalBytes += dataSize;
            printf("rdt_recv_file: Pacote recebido, seq %d (%d bytes).\n", p.h.pkt_seq, dataSize);
            
            make_pkt(&ack, PKT_ACK, p.h.pkt_seq, NULL, 0);
            sendto(sockfd, &ack, ack.h.pkt_size, 0,
                   (struct sockaddr *)&src, sizeof(struct sockaddr_in));
            _rcv_seqnum++;
        } else {
            // Caso o pacote esteja fora de ordem, reenvia o último ACK cumulativo.
            printf("rdt_recv_file: Pacote fora de ordem (esperado seq %d).\n", _rcv_seqnum);
            make_pkt(&ack, PKT_ACK, _rcv_seqnum - 1, NULL, 0);
            sendto(sockfd, &ack, ack.h.pkt_size, 0,
                   (struct sockaddr *)&src, sizeof(struct sockaddr_in));
        }
    }
    
    fclose(fp);
    printf("rdt_recv_file: Transferência concluída. Total de bytes recebidos: %d\n", totalBytes);
    return totalBytes;
}
