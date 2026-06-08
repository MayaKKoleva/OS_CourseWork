#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>

#define MAX_BUF 4096

static int sock_fd = -1;
static char sock_rbuf[MAX_BUF];
static int  sock_rbuf_len = 0;
static char stdin_rbuf[MAX_BUF];
static int  stdin_rbuf_len = 0;

static void send_line(const char *msg) {
    write(sock_fd, msg, strlen(msg));
}

static int readline_fd(int fd, char *fdbuf, int *fdlen, char *out, int outsize) {
    while (1) {
        for (int i = 0; i < *fdlen; i++) {
            if (fdbuf[i] == '\n') {
                int len = i < outsize - 1 ? i : outsize - 1;
                memcpy(out, fdbuf, len);
                out[len] = '\0';
                if (len > 0 && out[len-1] == '\r') out[--len] = '\0';
                memmove(fdbuf, fdbuf + i + 1, *fdlen - i - 1);
                *fdlen -= i + 1;
                return 1;
            }
        }
        int n = read(fd, fdbuf + *fdlen, MAX_BUF - *fdlen - 1);
        if (n <= 0) return 0;
        *fdlen += n;
    }
}

static int readline_sock(char *out, int outsize) {
    return readline_fd(sock_fd, sock_rbuf, &sock_rbuf_len, out, outsize);
}

static int wait_for_input(int want_stdin) {
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(sock_fd, &rset);
    int maxfd = sock_fd;
    if (want_stdin) {
        FD_SET(STDIN_FILENO, &rset);
        if (STDIN_FILENO > maxfd) maxfd = STDIN_FILENO;
    }
    int r = select(maxfd + 1, &rset, NULL, NULL, NULL);
    if (r <= 0) return 0;
    if (FD_ISSET(sock_fd, &rset)) return 1;
    if (want_stdin && FD_ISSET(STDIN_FILENO, &rset)) return 2;
    return 0;
}

static void display_state(const char *line) {
    char copy[MAX_BUF];
    strncpy(copy, line + 6, MAX_BUF - 1);
    copy[MAX_BUF - 1] = '\0';

    char *tokens[512];
    int ntok = 0;
    char *tok = strtok(copy, " ");
    while (tok && ntok < 511) {
        tokens[ntok++] = tok;
        tok = strtok(NULL, " ");
    }

    if (ntok < 2) {
        printf("Word: ?\nIncorrect guesses: \n");
        return;
    }

    char *inc_letters = tokens[ntok - 1];
    int word_len = ntok - 2;

    printf("Word: ");
    for (int i = 0; i < word_len; i++) printf("%s", tokens[i]);
    printf("\n");

    printf("Incorrect guesses: ");
    int first = 1;
    for (int i = 0; inc_letters[i]; i++) {
        if (inc_letters[i] == ',') { printf(", "); }
        else { if (!first) {} printf("%c", inc_letters[i]); first = 0; }
    }
    printf("\n");
    fflush(stdout);
}

static void format_inc(const char *csv, char *out, int outsize) {
    int pos = 0;
    for (int i = 0; csv[i] && pos < outsize - 2; i++) {
        if (csv[i] == ',') { out[pos++] = ','; out[pos++] = ' '; }
        else out[pos++] = csv[i];
    }
    out[pos] = '\0';
}

static void handle_result(const char *res_line) {
    const char *res = res_line + 7;
    char yourinc_line[MAX_BUF], oppinc_line[MAX_BUF];
    readline_sock(yourinc_line, sizeof(yourinc_line));
    readline_sock(oppinc_line, sizeof(oppinc_line));

    char your_inc[256] = "", opp_inc[256] = "";
    if (strncmp(yourinc_line, "YOURINC ", 8) == 0)
        format_inc(yourinc_line + 8, your_inc, sizeof(your_inc));
    if (strncmp(oppinc_line, "OPPINC ", 7) == 0)
        format_inc(oppinc_line + 7, opp_inc, sizeof(opp_inc));

    if (strcmp(res, "WIN") == 0) printf("YOU WIN! :)\n");
    else if (strcmp(res, "LOSE") == 0) printf("You Lose! :(\n");
    else printf("Tie :/\n");

    printf("Your incorrect guesses: %s\n", your_inc);
    printf("Opponent's incorrect guesses: %s\n", opp_inc);
    fflush(stdout);
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <host> <port> <word>\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];
    int port = atoi(argv[2]);
    const char *word_arg = argv[3];

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) { perror("socket"); return 1; }

    struct hostent *he = gethostbyname(host);
    if (!he) { fprintf(stderr, "Cannot resolve %s\n", host); return 1; }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }

    char msg[512];
    snprintf(msg, sizeof(msg), "WORD %s\n", word_arg);
    send_line(msg);

    int solved = 0;
    int awaiting_guess = 0;

    while (1) {
        int want_stdin = (!solved && awaiting_guess);
        int r = wait_for_input(want_stdin);

        if (r == 1 || !want_stdin) {
            char line[MAX_BUF];

            int found = 0;
            for (int i = 0; i < sock_rbuf_len; i++) {
                if (sock_rbuf[i] == '\n') { found = 1; break; }
            }
            if (!found && r != 1) {
                goto check_stdin;
            }

            if (!readline_sock(line, sizeof(line))) break;

            if (strncmp(line, "STATE ", 6) == 0) {
                display_state(line);
                awaiting_guess = 1;
            } else if (strcmp(line, "SOLVED") == 0) {
                solved = 1;
                awaiting_guess = 0;
            } else if (strncmp(line, "RESULT ", 7) == 0) {
                handle_result(line);
                break;
            } else if (strncmp(line, "ERROR ", 6) == 0) {
                fprintf(stderr, "Server error: %s\n", line + 6);
                close(sock_fd);
                return 1;
            }
            continue;
        }

check_stdin:
        if (r == 2 && awaiting_guess && !solved) {
            char guess_buf[64];
            if (!readline_fd(STDIN_FILENO, stdin_rbuf, &stdin_rbuf_len,
                             guess_buf, sizeof(guess_buf))) break;
            char g = guess_buf[0];
            if ('A' <= g && g <= 'Z') g = g - 'A' + 'a';
            snprintf(msg, sizeof(msg), "GUESS %c\n", g);
            send_line(msg);
            awaiting_guess = 0;
        }
    }

    close(sock_fd);
    return 0;
}