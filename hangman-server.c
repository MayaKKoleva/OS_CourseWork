#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include "game.h"

#define MAX_BUF 4096
#define MAX_WORD 256

typedef struct {
    int fd;
    char rbuf[MAX_BUF];
    int rbuf_len;
    char word[MAX_WORD];
    int word_ready;
} client_t;

static int readline(client_t *c, char *out, int outsize) {
    for (int i = 0; i < c->rbuf_len; i++) {
        if (c->rbuf[i] == '\n') {
            int len = i < outsize - 1 ? i : outsize - 1;
            memcpy(out, c->rbuf, len);
            out[len] = '\0';
            if (len > 0 && out[len-1] == '\r') out[--len] = '\0';
            memmove(c->rbuf, c->rbuf + i + 1, c->rbuf_len - i - 1);
            c->rbuf_len -= i + 1;
            return 1;
        }
    }
    return 0;
}

static void send_line(int fd, const char *msg) {
    write(fd, msg, strlen(msg));
}

static void send_state(int fd, secret_word_t *word) {
    char masked[MAX_WORD * 2];
    int pos = 0;
    for (size_t i = 0; i < word->word_length; i++) {
        if (i > 0) masked[pos++] = ' ';
        char ch = '_';
        secret_word_letter_at(word, i, &ch);
        if (secret_word_letter_at(word, i, NULL) == SECRET_WORD_LETTER_REVEALED) {
            char real = '_';
            secret_word_letter_at(word, i, &real);
            masked[pos++] = real;
        } else {
            masked[pos++] = '_';
        }
    }
    masked[pos] = '\0';

    char inc[64];
    int ipos = 0;
    for (char lc = 'a'; lc <= 'z'; lc++) {
        if (letter_set_contains(word->incorrect_guesses, lc)) {
            if (ipos > 0) inc[ipos++] = ',';
            inc[ipos++] = lc;
        }
    }
    inc[ipos] = '\0';
    if (ipos == 0)
        strcpy(inc, "0");

    char buf[MAX_BUF];
    snprintf(buf, sizeof(buf), "STATE %s %zu %s\n",
             masked,
             secret_word_incorrect_guess_count(word),
             inc);
    send_line(fd, buf);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);

    int srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(srv_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(srv_fd);
        return 1;
    }

    if (listen(srv_fd, 5) < 0) {
        perror("listen");
        close(srv_fd);
        return 1;
    }

    printf("Listening on %d...\n", port);
    fflush(stdout);

    client_t clients[2];
    memset(clients, 0, sizeof(clients));
    clients[0].fd = -1;
    clients[1].fd = -1;

    for (int i = 0; i < 2; i++) {
        clients[i].fd = accept(srv_fd, NULL, NULL);
        if (clients[i].fd < 0) {
            perror("accept");
            return 1;
        }
    }
    close(srv_fd); 

    int words_received = 0;
    while (words_received < 2) {
        fd_set rset;
        FD_ZERO(&rset);
        int maxfd = -1;
        for (int i = 0; i < 2; i++) {
            if (clients[i].fd >= 0 && !clients[i].word_ready) {
                FD_SET(clients[i].fd, &rset);
                if (clients[i].fd > maxfd) maxfd = clients[i].fd;
            }
        }
        select(maxfd + 1, &rset, NULL, NULL, NULL);

        for (int i = 0; i < 2; i++) {
            if (clients[i].fd < 0 || clients[i].word_ready) continue;
            if (!FD_ISSET(clients[i].fd, &rset)) continue;

            int n = read(clients[i].fd,
                         clients[i].rbuf + clients[i].rbuf_len,
                         MAX_BUF - clients[i].rbuf_len - 1);
            if (n <= 0) {
                close(clients[i].fd);
                clients[i].fd = -1;
                continue;
            }
            clients[i].rbuf_len += n;

            char line[MAX_BUF];
            while (readline(&clients[i], line, sizeof(line))) {
                if (strncmp(line, "WORD ", 5) == 0) {
                    char *w = line + 5;
                    secret_word_t tmp;
                    if (!secret_word_init_from_c_string(&tmp, w)) {
                        send_line(clients[i].fd, "ERROR Invalid word\n");
                        close(clients[i].fd);
                        clients[i].fd = -1;
                        break;
                    }
                    secret_word_free(&tmp);
                    strncpy(clients[i].word, w, MAX_WORD - 1);
                    clients[i].word_ready = 1;
                    words_received++;
                    break;
                }
            }
        }
    }

    if (clients[0].fd < 0 || clients[1].fd < 0) {
        fprintf(stderr, "A client disconnected before game started\n");
        return 1;
    }

    secret_word_t words[2];
    secret_word_init_from_c_string(&words[0], clients[1].word);
    secret_word_init_from_c_string(&words[1], clients[0].word); 

    send_state(clients[0].fd, &words[0]);
    send_state(clients[1].fd, &words[1]);

    int done[2] = {0, 0};

    while (!done[0] || !done[1]) {
        fd_set rset;
        FD_ZERO(&rset);
        int maxfd = -1;
        for (int i = 0; i < 2; i++) {
            if (!done[i] && clients[i].fd >= 0) {
                FD_SET(clients[i].fd, &rset);
                if (clients[i].fd > maxfd) maxfd = clients[i].fd;
            }
        }
        if (maxfd < 0) break;
        select(maxfd + 1, &rset, NULL, NULL, NULL);

        for (int i = 0; i < 2; i++) {
            if (done[i] || clients[i].fd < 0) continue;
            if (!FD_ISSET(clients[i].fd, &rset)) continue;

            int n = read(clients[i].fd,
                         clients[i].rbuf + clients[i].rbuf_len,
                         MAX_BUF - clients[i].rbuf_len - 1);
            if (n <= 0) {
                close(clients[i].fd);
                clients[i].fd = -1;
                done[i] = 1;
                continue;
            }
            clients[i].rbuf_len += n;

            char line[MAX_BUF];
            while (readline(&clients[i], line, sizeof(line))) {
                if (strncmp(line, "GUESS ", 6) != 0) continue;
                char guess = line[6];

                secret_word_guess_result_t res = secret_word_guess(&words[i], guess);
                (void)res;

                if (secret_word_is_solved(&words[i])) {
                    send_state(clients[i].fd, &words[i]);
                    send_line(clients[i].fd, "SOLVED\n");
                    done[i] = 1;
                } else {
                    send_state(clients[i].fd, &words[i]);
                }
            }
        }
    }

    size_t inc0 = secret_word_incorrect_guess_count(&words[0]);
    size_t inc1 = secret_word_incorrect_guess_count(&words[1]);

    char result0[16], result1[16];
    if (inc0 < inc1) {
        strcpy(result0, "WIN");
        strcpy(result1, "LOSE");
    } else if (inc1 < inc0) {
        strcpy(result0, "LOSE");
        strcpy(result1, "WIN");
    } else {
        strcpy(result0, "TIE");
        strcpy(result1, "TIE");
    }

    char inc_str0[64], inc_str1[64];
    int p0 = 0, p1 = 0;
    for (char lc = 'a'; lc <= 'z'; lc++) {
        if (letter_set_contains(words[0].incorrect_guesses, lc)) {
            if (p0 > 0) inc_str0[p0++] = ',';
            inc_str0[p0++] = lc;
        }
        if (letter_set_contains(words[1].incorrect_guesses, lc)) {
            if (p1 > 0) inc_str1[p1++] = ',';
            inc_str1[p1++] = lc;
        }
    }
    inc_str0[p0] = '\0';
    inc_str1[p1] = '\0';

    char buf[256];

    if (clients[0].fd >= 0) {
        snprintf(buf, sizeof(buf), "RESULT %s\n", result0);
        send_line(clients[0].fd, buf);
        snprintf(buf, sizeof(buf), "YOURINC %s\n", inc_str0);
        send_line(clients[0].fd, buf);
        snprintf(buf, sizeof(buf), "OPPINC %s\n", inc_str1);
        send_line(clients[0].fd, buf);
        close(clients[0].fd);
    }

    if (clients[1].fd >= 0) {
        snprintf(buf, sizeof(buf), "RESULT %s\n", result1);
        send_line(clients[1].fd, buf);
        snprintf(buf, sizeof(buf), "YOURINC %s\n", inc_str1);
        send_line(clients[1].fd, buf);
        snprintf(buf, sizeof(buf), "OPPINC %s\n", inc_str0);
        send_line(clients[1].fd, buf);
        close(clients[1].fd);
    }

    secret_word_free(&words[0]);
    secret_word_free(&words[1]);
    return 0;
}