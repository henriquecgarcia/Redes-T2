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

// Configurações da janela e timeout estático padrão.
#define STATIC_WINDOW_SIZE 5
#define MAX_DYNAMIC_WINDOW 20
#define MIN_DYNAMIC_WINDOW 1
#define TIMEOUT_SEC        2
#define TIMEOUT_USEC       1

// Variáveis globais para sequência (definidas como extern em rdt.h).
int biterror_inject = FALSE;
hseq_t _snd_seqnum = 1;
hseq_t _rcv_seqnum = 1;

// Variáveis globais para a janela de transmissão dinâmica.
int dynamic_window_enabled = FALSE;   // 0 = janela estática, 1 = janela dinâmica
int current_window_size = STATIC_WINDOW_SIZE;

// Variáveis globais para timeout: se dinâmico, serão ajustados.
int dynamic_timeout_enabled = TRUE;    // 0 = timeout estático, 1 = timeout dinâmico
int current_timeout_sec = TIMEOUT_SEC;
int current_timeout_usec = TIMEOUT_USEC;
const int MAX_TIMEOUT_SEC = 10;     // valor máximo de timeout
const int MIN_TIMEOUT_SEC = TIMEOUT_SEC; // valor mínimo de timeout

// Nova flag para ativar ou desativar o fast retransmit.
// 1 = fast retransmit ativado, 0 = fast retransmit desativado.
int fast_retransmit_enabled = FALSE;

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

// Cria um pacote com o header, copia o payload (se houver) e calcula o checksum.
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

// Função rdt_send: envia um buffer segmentado usando uma janela de transmissão.
// Se dynamic_window_enabled for 1, a janela é ajustada dinamicamente.
// O fast retransmit é acionado se a flag fast_retransmit_enabled estiver ativada.
int rdt_send(int sockfd, void *buf, int buf_len, struct sockaddr_in *dst) {
    int chunk_size = MAX_MSG_LEN;
    int num_segments = (buf_len + chunk_size - 1) / chunk_size;
    
    pkt *packets = malloc(num_segments * sizeof(pkt));
    if (!packets) {
        perror("rdt_send: malloc");
        return ERROR;
    }
    
    // Criação dos pacotes com os dados.
    for (int i = 0; i < num_segments; i++) {
        int offset = i * chunk_size;
        int remaining = buf_len - offset;
        int seg_len = (remaining > chunk_size) ? chunk_size : remaining;
        if (make_pkt(&packets[i], PKT_DATA, _snd_seqnum + i, (char *)buf + offset, seg_len) < 0) {
            free(packets);
            return ERROR;
        }
    }
    
    // Ajusta a janela de transmissão: se dinâmica, usa current_window_size; caso contrário, STATIC_WINDOW_SIZE.
    current_window_size = dynamic_window_enabled ? current_window_size : STATIC_WINDOW_SIZE;
    
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
    hseq_t fastRetransmittedSeq = 0;
    
    while (base < num_segments) {
        // Envia os pacotes dentro da janela.
        while (next_seq < num_segments && next_seq < base + current_window_size) {
            pkt temp_pkt;
            memcpy(&temp_pkt, &packets[next_seq], sizeof(pkt));
            
            // Injeção de erro (aplicada de forma randômica, se biterror_inject estiver ativo)
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
        timeout.tv_sec = current_timeout_sec;
        timeout.tv_usec = current_timeout_usec;
        
        int rv = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
        if (rv < 0) {
            perror("rdt_send: select error");
            free(packets);
            return ERROR;
        } else if (rv == 0) {
            printf("rdt_send: Timeout. Retransmitindo a partir do pacote seq %d\n", packets[base].h.pkt_seq);
            next_seq = base;
            if (dynamic_timeout_enabled) {
                current_timeout_sec *= 2;
                if (current_timeout_sec > MAX_TIMEOUT_SEC)
                    current_timeout_sec = MAX_TIMEOUT_SEC;
                printf("rdt_send: Novo timeout dinâmico: %d segundos\n", current_timeout_sec);
            }
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
            
            // Se o fast retransmit estiver habilitado, processa os ACKs duplicados.
            if (fast_retransmit_enabled) {
                if (ack.h.pkt_seq == last_ack_seq) {
                    if (fastRetransmittedSeq != ack.h.pkt_seq) {
                        dup_ack_count++;
                        printf("rdt_send: ACK duplicado (%d) para o pacote seq %d\n", dup_ack_count, ack.h.pkt_seq);
                        if (dup_ack_count >= 3) {
                            printf("rdt_send: Fast retransmission disparada para o pacote seq %d\n", packets[base].h.pkt_seq);
                            next_seq = base;
                            fastRetransmittedSeq = ack.h.pkt_seq;
                            dup_ack_count = 0;
                            continue;
                        }
                    } else {
                        printf("rdt_send: ACK duplicado para o mesmo pacote (seq %d) já retransmitido, ignorando.\n", ack.h.pkt_seq);
                    }
                } else if (ack.h.pkt_seq > last_ack_seq) {
                    last_ack_seq = ack.h.pkt_seq;
                    dup_ack_count = 0;
                    fastRetransmittedSeq = 0;
                    int ack_index = ack.h.pkt_seq - _snd_seqnum;
                    if (ack_index >= base && ack_index < num_segments) {
                        printf("rdt_send: ACK recebido para o pacote seq %d\n", ack.h.pkt_seq);
                        base = ack_index + 1;
                        if (dynamic_timeout_enabled && current_timeout_sec > MIN_TIMEOUT_SEC) {
                            current_timeout_sec--;
                            if (current_timeout_sec < MIN_TIMEOUT_SEC)
                                current_timeout_sec = MIN_TIMEOUT_SEC;
                            printf("rdt_send: Timeout dinâmico reduzido para %d segundos\n", current_timeout_sec);
                        }
                        if (dynamic_window_enabled && current_window_size < MAX_DYNAMIC_WINDOW) {
                            current_window_size++;
                            printf("rdt_send: Janela dinâmica aumentada para %d\n", current_window_size);
                        }
                    }
                }
            } else {
                // Se fast retransmit estiver desativado, ignoramos a contagem de ACKs duplicados.
                if (ack.h.pkt_seq > last_ack_seq) {
                    last_ack_seq = ack.h.pkt_seq;
                    int ack_index = ack.h.pkt_seq - _snd_seqnum;
                    if (ack_index >= base && ack_index < num_segments) {
                        printf("rdt_send: ACK recebido para o pacote seq %d\n", ack.h.pkt_seq);
                        base = ack_index + 1;
                    }
                }
            }
        }
    }
    _snd_seqnum += num_segments;
    free(packets);
    return buf_len;
}

// Função rdt_recv: recebe um único pacote de dados (usado para mensagens).
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

// Função rdt_send_file: envia um arquivo inteiro incluindo o PKT_START (metadados),
// os dados segmentados via rdt_send e finaliza com handshake de terminação.
int rdt_send_file(int sockfd, const char *filename, struct sockaddr_in *dst) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("rdt_send_file: fopen");
        return ERROR;
    }
    
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
                    (struct sockaddr *)dst, sizeof(struct sockaddr_in));
    if (ns < 0) {
        perror("rdt_send_file: sendto(PKT_START)");
        fclose(fp);
        return ERROR;
    }
    printf("rdt_send_file: PKT_START enviado (seq %d). Nome: %s, Tamanho: %ld bytes.\n", 
           startPkt.h.pkt_seq, meta.filename, meta.fileSize);
    
    // Reinicia a numeração para os pacotes de dados: o receptor espera que o primeiro pacote seja seq 1.
    _snd_seqnum = 1;
    
    // Lê o arquivo para um buffer (para arquivos grandes, adapte para leitura em blocos).
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
    
    int sent = rdt_send(sockfd, fileBuffer, fileSize, dst);
    if (sent < 0) {
        free(fileBuffer);
        return ERROR;
    }
    free(fileBuffer);
    
    // Envia FIN do cliente.
    pkt finPkt;
    if (make_pkt(&finPkt, PKT_FIN, _snd_seqnum, NULL, 0) < 0)
        return ERROR;
    
    ns = sendto(sockfd, &finPkt, finPkt.h.pkt_size, 0,
                (struct sockaddr *)dst, sizeof(struct sockaddr_in));
    if (ns < 0) {
        perror("rdt_send_file: sendto(PKT_FIN)");
        return ERROR;
    }
    printf("rdt_send_file: Pacote FIN enviado (seq %d).\n", finPkt.h.pkt_seq);
    
    // Aguarda ACK para o FIN enviado.
    pkt ack;
    struct timeval timeout = {current_timeout_sec, current_timeout_usec};
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
    
    // Fase 2: Aguarda FIN do servidor e envia ACK para ele.
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    timeout.tv_sec = current_timeout_sec;
    timeout.tv_usec = current_timeout_usec;
    rv = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
    if (rv > 0) {
        socklen_t addrlen = sizeof(struct sockaddr_in);
        ns = recvfrom(sockfd, &ack, sizeof(pkt), 0, NULL, &addrlen);
        if (ns < 0) {
            perror("rdt_send_file: recvfrom(PKT_FIN do servidor)");
            return ERROR;
        }
        if (ack.h.pkt_type == PKT_FIN) {
            printf("rdt_send_file: FIN recebido do servidor (seq %d).\n", ack.h.pkt_seq);
            if (make_pkt(&ack, PKT_ACK, ack.h.pkt_seq, NULL, 0) < 0)
                return ERROR;
            ns = sendto(sockfd, &ack, ack.h.pkt_size, 0,
                        (struct sockaddr *)dst, sizeof(struct sockaddr_in));
            if (ns < 0) {
                perror("rdt_send_file: sendto(PKT_ACK para FIN do servidor)");
                return ERROR;
            }
            printf("rdt_send_file: ACK enviado para o FIN do servidor.\n");
        }
    } else {
        printf("rdt_send_file: Timeout aguardando FIN do servidor.\n");
    }
    
    return sent;
}

// Função rdt_recv_file: recebe um arquivo e grava no sistema de arquivos.
// O receptor espera inicialmente um PKT_START com metadados.
int rdt_recv_file(int sockfd, const char *filename) {
    FILE *fp = NULL;
    pkt p, ack;
    struct sockaddr_in src;
    socklen_t addrlen;
    fd_set readfds;
    struct timeval timeout;
    int ns, nr, rv;
    int totalBytes = 0;
    
    // Aguarda o PKT_START com os metadados do arquivo.
    addrlen = sizeof(struct sockaddr_in);
    nr = recvfrom(sockfd, &p, sizeof(pkt), 0, (struct sockaddr *)&src, &addrlen);
    if (nr < 0) {
        perror("rdt_recv_file: recvfrom(PKT_START)");
        return ERROR;
    }
    if (p.h.pkt_type != PKT_START) {
        fprintf(stderr, "rdt_recv_file: Esperado PKT_START, recebido outro tipo.\n");
        return ERROR;
    }
    // Extrai os metadados.
    file_meta meta;
    if (p.h.pkt_size - sizeof(hdr) < sizeof(file_meta)) {
        fprintf(stderr, "rdt_recv_file: Tamanho insuficiente para metadados.\n");
        return ERROR;
    }
    memcpy(&meta, p.msg, sizeof(file_meta));
    printf("rdt_recv_file: PKT_START recebido. Nome do arquivo: %s, Tamanho: %ld bytes.\n", meta.filename, meta.fileSize);
    // (Opcional) Envia ACK para o PKT_START.
    if (make_pkt(&ack, PKT_ACK, p.h.pkt_seq, NULL, 0) < 0)
        return ERROR;
    ns = sendto(sockfd, &ack, ack.h.pkt_size, 0, (struct sockaddr *)&src, sizeof(struct sockaddr_in));
    if (ns < 0) {
        perror("rdt_recv_file: sendto(PKT_START ACK)");
        return ERROR;
    }
    
    // Abre o arquivo para escrita; utiliza o nome recebido nos metadados.
    fp = fopen(meta.filename, "wb");
    if (!fp) {
        perror("rdt_recv_file: fopen");
        return ERROR;
    }
    
    // Recebe os pacotes de dados.
    while (1) {
        addrlen = sizeof(struct sockaddr_in);
        nr = recvfrom(sockfd, &p, sizeof(pkt), 0, (struct sockaddr *)&src, &addrlen);
        if (nr < 0) {
            perror("rdt_recv_file: recvfrom()");
            fclose(fp);
            return ERROR;
        }
        
        if (iscorrupted(&p)) {
            printf("rdt_recv_file: Pacote corrompido, reenviando último ACK.\n");
            if (make_pkt(&ack, PKT_ACK, _rcv_seqnum - 1, NULL, 0) < 0) {
                fclose(fp);
                return ERROR;
            }
            sendto(sockfd, &ack, ack.h.pkt_size, 0,
                   (struct sockaddr *)&src, sizeof(struct sockaddr_in));
            continue;
        }
        
        // Se for um pacote FIN, inicia o handshake de terminação.
        if (p.h.pkt_type == PKT_FIN) {
            if (make_pkt(&ack, PKT_ACK, p.h.pkt_seq, NULL, 0) < 0) {
                fclose(fp);
                return ERROR;
            }
            sendto(sockfd, &ack, ack.h.pkt_size, 0,
                   (struct sockaddr *)&src, sizeof(struct sockaddr_in));
            printf("rdt_recv_file: FIN recebido do cliente. ACK enviado para FIN.\n");
            
            // Envia FIN do servidor.
            pkt serverFin;
            if (make_pkt(&serverFin, PKT_FIN, _snd_seqnum, NULL, 0) < 0) {
                fclose(fp);
                return ERROR;
            }
            ns = sendto(sockfd, &serverFin, serverFin.h.pkt_size, 0,
                        (struct sockaddr *)&src, sizeof(struct sockaddr_in));
            if (ns < 0) {
                perror("rdt_recv_file: sendto(PKT_FIN do servidor)");
                fclose(fp);
                return ERROR;
            }
            printf("rdt_recv_file: FIN enviado pelo servidor (seq %d).\n", serverFin.h.pkt_seq);
            
            // (Opcional) Aguarda ACK para o FIN do servidor.
            FD_ZERO(&readfds);
            FD_SET(sockfd, &readfds);
            timeout.tv_sec = current_timeout_sec;
            timeout.tv_usec = current_timeout_usec;
            rv = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
            if (rv > 0) {
                socklen_t ackAddrLen = sizeof(struct sockaddr_in);
                nr = recvfrom(sockfd, &ack, sizeof(pkt), 0, (struct sockaddr *)&src, &ackAddrLen);
                if (nr > 0 && ack.h.pkt_type == PKT_ACK && ack.h.pkt_seq == serverFin.h.pkt_seq) {
                    printf("rdt_recv_file: ACK recebido para o FIN do servidor.\n");
                }
            }
            break;
        }
        
        if (p.h.pkt_type == PKT_DATA && p.h.pkt_seq == _rcv_seqnum) {
            int dataSize = p.h.pkt_size - sizeof(hdr);
            if (fwrite(p.msg, 1, dataSize, fp) != dataSize) {
                perror("rdt_recv_file: fwrite");
                fclose(fp);
                return ERROR;
            }
            totalBytes += dataSize;
            printf("rdt_recv_file: Pacote recebido, seq %d (%d bytes).\n", p.h.pkt_seq, dataSize);
            
            if (make_pkt(&ack, PKT_ACK, p.h.pkt_seq, NULL, 0) < 0) {
                fclose(fp);
                return ERROR;
            }
            sendto(sockfd, &ack, ack.h.pkt_size, 0,
                   (struct sockaddr *)&src, sizeof(struct sockaddr_in));
            _rcv_seqnum++;
        } else {
            printf("rdt_recv_file: Pacote fora de ordem (esperado seq %d).\n", _rcv_seqnum);
            if (make_pkt(&ack, PKT_ACK, _rcv_seqnum - 1, NULL, 0) < 0) {
                fclose(fp);
                return ERROR;
            }
            sendto(sockfd, &ack, ack.h.pkt_size, 0,
                   (struct sockaddr *)&src, sizeof(struct sockaddr_in));
        }
    }
    
    fclose(fp);
    printf("rdt_recv_file: Transferência concluída. Total de bytes recebidos: %d\n", totalBytes);
    return totalBytes;
}
