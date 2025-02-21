#ifndef RDT_H
#define RDT_H

#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>

// Tamanho máximo do payload de cada pacote.
#define MAX_MSG_LEN 1024

// Definições de valores lógicos e de retorno.
#define TRUE    1
#define FALSE   0
#define ERROR  -1
#define SUCCESS  0

// Tipos de pacotes:
typedef enum {
    PKT_DATA  = 0,   // Pacote de dados
    PKT_ACK   = 1,   // Acknowledgment
    PKT_FIN   = 2,   // Indica fim da transmissão do arquivo
    PKT_START = 3    // Pacote de início, contendo metadados do arquivo
} htype_t;

// Definição do tipo de sequência.
typedef uint32_t hseq_t;

// Estrutura do header do pacote.
typedef struct {
    int    pkt_size;    // Tamanho total do pacote (header + payload)
    unsigned short csum; // Checksum do pacote (para detecção de erros)
    htype_t pkt_type;   // Tipo do pacote (DATA, ACK, FIN ou START)
    hseq_t  pkt_seq;    // Número de sequência do pacote
} hdr;

// Estrutura do pacote, com header e espaço para payload.
typedef struct { 
    hdr h;
    char msg[MAX_MSG_LEN];
} pkt;

// Estrutura para metadados do arquivo (usada no PKT_START)
typedef struct {
    char filename[256];
    long fileSize;
} file_meta;

// Declaração das funções do protocolo.
unsigned short checksum(unsigned short *buf, int nbytes);
int iscorrupted(pkt *pr);
int make_pkt(pkt *p, htype_t type, hseq_t seqnum, void *msg, int msg_len);
int rdt_send(int sockfd, void *buf, int buf_len, struct sockaddr_in *dst);
int rdt_recv(int sockfd, void *buf, int buf_len, struct sockaddr_in *src);
int rdt_close(int sockfd, struct sockaddr_in *dst, int snd_seqnum);
int rdt_recv_file(int sockfd, const char *filename);

// Variáveis globais para gerenciar a sequência.
extern int biterror_inject;
extern hseq_t _snd_seqnum;
extern hseq_t _rcv_seqnum;

// Variáveis globais para a janela de transmissão dinâmica.
extern int dynamic_window_enabled;  // 0 = janela estática, 1 = janela dinâmica
extern int current_window_size;

#endif
