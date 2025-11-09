#include “player.h”
#include "udp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

int receive_udp_song(char *song_out, size_t len) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr = {0}, client = {0};
    socklen_t clen = sizeof(client);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(UDP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(sock); return -1;
    }

    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[1024];
    printf("Waiting for UDP JSON on port %d (30s timeout)...\n", UDP_PORT);
    ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&client, &clen);
    if (n <= 0) {
        perror("recvfrom"); close(sock); return -1;
    }
    buf[n] = '\0';
    printf("Received UDP: %s\n", buf);

    char *p = strstr(buf, "\"song\"");
    if (!p || sscanf(p, "\"song\"%*[: ]\"%[^\"]\"", song_out) != 1) {
        fprintf(stderr, "Invalid JSON or missing 'song'.\n");
        close(sock);
        return -1;
    }

    char ack[256];
    snprintf(ack, sizeof(ack), "{\"ack\":\"ok\",\"song\":\"%s\"}", song_out);
    sendto(sock, ack, strlen(ack), 0, (struct sockaddr *)&client, clen);

    printf("Parsed song name: '%s'\n", song_out);
    close(sock);
    return 0;
}

void emulate_udp_from_file(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        perror("emulate_udp_from_file open");
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char song[128] = {0};

        // Parse very simple JSON: {"song":"name"}
        char *p = strstr(line, "\"song\"");
        if (!p) continue;
        p = strchr(p, ':');
        if (!p) continue;
        p++;
        while (*p && (*p == ' ' || *p == '"' || *p == '\'')) p++;
        char *end = p;
        while (*end && *end != '"' && *end != '\'' && *end != '}') end++;
        *end = '\0';
        strncpy(song, p, sizeof(song)-1);

        printf("Emulated UDP: received song '%s'\n", song);
        play_song(song);  // use your existing playback logic
    }

    fclose(f);
}
