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

// Definições para a janela e timeout
#define WINDOW_SIZE   5
#define TIMEOUT_SEC   10
#define TIMEOUT_USEC  1

int biterror_inject = FALSE;
hseq_t _snd_seqnum = 1;
hseq_t _rcv_seqnum = 1;

// Definindo a função de checksum, que calcula a soma de verificação de um buffer de bytes
unsigned short checksum(unsigned short *buf, int nbytes){
    register long sum = 0;
    while (nbytes > 1) {
        sum += *(buf++);
        nbytes -= 2;
    }
    if (nbytes == 1)
        sum += *(unsigned short *) buf;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (unsigned short) ~sum;
}

// Verifica se o pacote está corrompido
int iscorrupted(pkt *pr){
    pkt pl = *pr;
    pl.h.csum = 0;
    unsigned short csuml = checksum((unsigned short *)&pl, pl.h.pkt_size);
    if (csuml != pr->h.csum){
        return TRUE;
    }
    return FALSE;
}

// Monta um pacote: inclui o header, copia a mensagem (se houver) e calcula o checksum.
int make_pkt(pkt *p, htype_t type, hseq_t seqnum, void *msg, int msg_len) {
    if (msg_len > MAX_MSG_LEN) {
        printf("make_pkt: tamanho da msg (%d) maior que limite (%d).\n",
            msg_len, MAX_MSG_LEN);
        return ERROR;
    }
	// Montando o pacote em si
    p->h.pkt_size = sizeof(hdr);
    p->h.csum = 0;
    p->h.pkt_type = type;
    p->h.pkt_seq = seqnum;
    if (msg_len > 0) {
        p->h.pkt_size += msg_len;
        memset(p->msg, 0, MAX_MSG_LEN);
        memcpy(p->msg, msg, msg_len);
    }
    p->h.csum = checksum((unsigned short *)p, p->h.pkt_size);
    return SUCCESS;
}

int has_ackseq(pkt *p, hseq_t seqnum) {
    if (p->h.pkt_type != PKT_ACK || p->h.pkt_seq != seqnum)
        return FALSE;
    return TRUE;
}

// ==========================================================================
// Nova implementação de rdt_send com janela de transmissão e timeout
// Neste exemplo, utilizamos Go-Back-N para enviar (possivelmente) vários
// segmentos de uma mensagem. Se a mensagem for pequena, teremos apenas um pacote.
// ==========================================================================
int rdt_send(int sockfd, void *buf, int buf_len, struct sockaddr_in *dst) {
    int chunk_size = MAX_MSG_LEN;  // cada pacote pode transportar até MAX_MSG_LEN bytes de payload
    int num_segments = (buf_len + chunk_size - 1) / chunk_size;
    
    // Aloca um vetor de pacotes para armazenar todos os segmentos
    pkt *packets = malloc(num_segments * sizeof(pkt));
    if (!packets) {
        perror("rdt_send: malloc");
        return ERROR;
    }
    
    // Segmenta a mensagem e cria os pacotes com sequências consecutivas
    for (int i = 0; i < num_segments; i++) {
        int offset = i * chunk_size;
        int remaining = buf_len - offset;
        int seg_len = (remaining > chunk_size) ? chunk_size : remaining;
        if (make_pkt(&packets[i], PKT_DATA, _snd_seqnum + i, (char *)buf + offset, seg_len) < 0) {
            free(packets);
            return ERROR;
        }
        // Se estiver ativado o modo de injeção de erro, corrompe a mensagem do pacote
        if (biterror_inject) {
            memset(packets[i].msg, 0, MAX_MSG_LEN);
        }
    }
    
    int base = 0;       // índice do primeiro pacote não confirmado
    int next_seq = 0;   // próximo índice a ser enviado
    struct timeval timeout;
    fd_set readfds;
    int ns, nr;
    struct sockaddr_in ack_addr;
    socklen_t addrlen;
    
    // Loop principal: enquanto houver pacotes a serem confirmados
    while (base < num_segments) {
        // Envia todos os pacotes que ainda não foram enviados e que cabem na janela
        while (next_seq < num_segments && next_seq < base + WINDOW_SIZE) {
            ns = sendto(sockfd, &packets[next_seq], packets[next_seq].h.pkt_size, 0,
                        (struct sockaddr *)dst, sizeof(struct sockaddr_in));
            if (ns < 0) {
                perror("rdt_send: sendto(PKT_DATA)");
                free(packets);
                return ERROR;
            }
            printf("rdt_send: Enviado pacote seq %d\n", packets[next_seq].h.pkt_seq);
            next_seq++;
        }
        
        // Prepara o select() para aguardar um ACK com timeout
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
            // Timeout: nenhum ACK chegou. Retransmite a janela (a partir do 'base')
            printf("rdt_send: Timeout, retransmitindo a partir do pacote seq %d\n",
                   packets[base].h.pkt_seq);
            next_seq = base; // reseta para retransmitir todos os pacotes não confirmados
            continue;
        } else {
            // Recebe o ACK
            pkt ack;
            addrlen = sizeof(ack_addr);
            nr = recvfrom(sockfd, &ack, sizeof(pkt), 0,
                          (struct sockaddr *)&ack_addr, &addrlen);
            if (nr < 0) {
                perror("rdt_send: recvfrom(PKT_ACK)");
                free(packets);
                return ERROR;
            }
            // Verifica se o ACK está corrompido ou se não é mesmo um ACK
            if (iscorrupted(&ack) || ack.h.pkt_type != PKT_ACK) {
                printf("rdt_send: ACK corrompido ou tipo incorreto recebido.\n");
                continue;
            }
            // Em Go-Back-N, o ACK é cumulativo: ele confirma todos os pacotes até aquele número.
            // Calcula o índice do pacote confirmado:
            int ack_index = ack.h.pkt_seq - _snd_seqnum;
            if (ack_index >= base && ack_index < num_segments) {
                printf("rdt_send: Recebido ACK para pacote seq %d\n", ack.h.pkt_seq);
                // Desliza a janela: todos os pacotes até ack_index foram confirmados
                base = ack_index + 1;
            }
        }
    }
    // Atualiza o número de sequência do remetente para as próximas transmissões
    _snd_seqnum += num_segments;
    free(packets);
    return buf_len;
}

// ==========================================================================
// Implementação do rdt_recv (lado receptor)
// Nesta versão (Go-Back-N), o receptor aceita apenas o pacote com a seq. esperada
// e, caso receba pacote fora de ordem ou corrompido, reenvia o último ACK cumulativo.
// ==========================================================================
int has_dataseqnum(pkt *p, hseq_t seqnum) {
    if (p->h.pkt_seq != seqnum || p->h.pkt_type != PKT_DATA)
        return FALSE;
    return TRUE;
}

int rdt_recv(int sockfd, void *buf, int buf_len, struct sockaddr_in *src) {
    pkt p, ack;
    int nr, ns;
    socklen_t addrlen;
    
    // Prepara um ACK para o último pacote recebido (inicialmente _rcv_seqnum - 1)
    if (make_pkt(&ack, PKT_ACK, _rcv_seqnum - 1, NULL, 0) < 0)
        return ERROR;

rerecv:
    addrlen = sizeof(struct sockaddr_in);
    nr = recvfrom(sockfd, &p, sizeof(pkt), 0, (struct sockaddr*)src,
                  &addrlen);
    if (nr < 0) {
        perror("rdt_recv: recvfrom()");
        return ERROR;
    }
    // Se o pacote estiver corrompido ou não for o esperado, reenvia o último ACK e aguarda novamente
    if (iscorrupted(&p) || !has_dataseqnum(&p, _rcv_seqnum)) {
        printf("rdt_recv: Pacote corrompido ou fora de ordem (esperado seq %d).\n", _rcv_seqnum);
        ns = sendto(sockfd, &ack, ack.h.pkt_size, 0,
                    (struct sockaddr*)src, sizeof(struct sockaddr_in));
        if (ns < 0) {
            perror("rdt_recv: sendto(PKT_ACK - 1)");
            return ERROR;
        }
        goto rerecv;
    }
    // Copia o payload para o buffer do usuário
    int msg_size = p.h.pkt_size - sizeof(hdr);
    if (msg_size > buf_len) {
        printf("rdt_recv(): Buffer insuficiente (%d) para payload (%d).\n", buf_len, msg_size);
        return ERROR;
    }
    memcpy(buf, p.msg, msg_size);
    
    // Prepara e envia o ACK para o pacote recebido
    if (make_pkt(&ack, PKT_ACK, p.h.pkt_seq, NULL, 0) < 0)
        return ERROR;
    ns = sendto(sockfd, &ack, ack.h.pkt_size, 0,
                (struct sockaddr*)src, sizeof(struct sockaddr_in));
    if (ns < 0) {
        perror("rdt_recv: sendto(PKT_ACK)");
        return ERROR;
    }
    printf("rdt_recv: Recebido pacote seq %d e enviado ACK\n", p.h.pkt_seq);
    _rcv_seqnum++;
    return msg_size;
}
