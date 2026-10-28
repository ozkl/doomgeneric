// SERSETUP.EXE, built in: two players over a serial line, the same bytes as
// id's serial driver (sersrc/SERSETUP.C, released under the GPL), so this
// plays a DOS machine running SERSETUP and the original DOOM.EXE (or
// ../msdos's doom.exe) over a null-modem cable or modems.
//
//   -serial DEVICE         a serial port: /dev/ttyS0, /dev/ttyUSB0...
//                          8N1 at 14400 baud as SERSETUP, or -9600, -19200,
//                          -38400, -57600, -115200 (both ends the same)
//   -serial HOST:PORT      a TCP connection instead of the cable: to another
//   -serial listen:PORT    copy of this port, or to DOSBox's serial port
//                          (serial1=nullmodem ... transparent:1)
//   -dial NUMBER / -answer with a modem on DEVICE; modem.cfg as SERSETUP's
//                          (init string, hang-up string, baud rate), or ATZ
//   -player1 / -player2    who is who (otherwise worked out)
//
// Packets: the bytes, each 70h doubled, then 70h 00. Joining: each side
// sends "ID<6 digits>_<stage>" every second until both have seen the
// other's; the lower ID is player 1.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <string>
// termios2: any baud rate (14400 has no Bxxx constant). Not with <termios.h>.
#include <asm/termios.h>
#include <asm/ioctls.h>
extern "C" int ioctl(int, unsigned long, ...);
#include "net_linux.h"

#define MAXPACKET 512
#define FRAMECHAR 0x70

static int fd = -1;
static bool is_tty;
static struct termios2 tty_saved;
static bool tty_changed;
static bool usemodem;
static char startup[256] = "ATZ", hangup[256] = "ATZ", baudrate[256];

static uint32_t ms_now() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// ---------------------------------------------------------------- the line
// Bytes in (inque) and out (outque), as PORT.C's interrupt-fed queues.
static uint8_t inque[8192];
static int in_head, in_tail;                 // in_head: next free
static std::string outque;

static void flush_out() {
    while (!outque.empty()) {
        ssize_t n = write(fd, outque.data(), outque.size());
        if (n <= 0) return;
        outque.erase(0, (size_t)n);
    }
}

static void fill_in() {
    flush_out();
    for (;;) {
        if (in_head - in_tail > (int)sizeof inque - 512) return;   // full: leave it in the OS
        uint8_t b[512];
        ssize_t n = read(fd, b, sizeof b);
        if (n <= 0) return;
        for (ssize_t i = 0; i < n; i++) inque[(in_head++) % sizeof inque] = b[i];
    }
}

static int read_byte() {
    if (in_tail == in_head) fill_in();
    if (in_tail == in_head) return -1;
    return inque[(in_tail++) % sizeof inque];
}

static void write_buffer(const char* b, size_t n) {
    // SERSETUP: if this would overrun the buffer, throw everything else out.
    if (outque.size() + n > 4096) outque.clear();
    outque.append(b, n);
    flush_out();
}

static void wait_line(int ms) {
    struct pollfd p = {fd, (short)(POLLIN | (outque.empty() ? 0 : POLLOUT)), 0};
    poll(&p, 1, ms);
}

// ---------------------------------------------------------------- packets
static uint8_t packet[MAXPACKET + 1];
static int packetlen;
static bool inescape, newpacket = true;

static bool ReadPacket() {
    if (in_head - in_tail > (int)sizeof inque - 4) {    // overflowed: throw it all out
        in_tail = in_head;
        newpacket = true;
        return false;
    }
    if (newpacket) { packetlen = 0; newpacket = false; }
    for (;;) {
        int c = read_byte();
        if (c < 0) return false;                    // not a whole packet yet
        if (inescape) {
            inescape = false;
            if (c != FRAMECHAR) { newpacket = true; return true; }
        } else if (c == FRAMECHAR) {
            inescape = true;
            continue;                               // a terminator or a literal 70h
        }
        if (packetlen >= MAXPACKET) continue;       // oversize
        packet[packetlen++] = (uint8_t)c;
    }
}

static void WritePacket(const uint8_t* b, int len) {
    char out[MAXPACKET * 2 + 2];
    int k = 0;
    if (len > MAXPACKET) return;
    while (len--) {
        if (*b == FRAMECHAR) out[k++] = FRAMECHAR;
        out[k++] = (char)*b++;
    }
    out[k++] = FRAMECHAR;
    out[k++] = 0;
    write_buffer(out, k);
}

// ---------------------------------------------------------------- opening
static bool open_tty(const char* dev, unsigned baud) {
    fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "Serial: %s: %s%s\n", dev, strerror(errno),
                errno == EACCES ? " (add yourself to the dialout group)" : "");
        return false;
    }
    struct termios2 t;
    if (ioctl(fd, TCGETS2, &t) != 0) { fprintf(stderr, "Serial: %s isn't a serial port\n", dev); return false; }
    tty_saved = t;
    tty_changed = true;
    // Raw, 8N1, no flow control (SERSETUP uses none), the modem lines on.
    t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY);
    t.c_oflag &= ~OPOST;
    t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS | CBAUD);
    t.c_cflag |= CS8 | CREAD | CLOCAL | BOTHER;
    t.c_cflag &= ~(CBAUD << IBSHIFT);
    t.c_cflag |= BOTHER << IBSHIFT;
    t.c_ispeed = t.c_ospeed = baud;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (ioctl(fd, TCSETS2, &t) != 0) { fprintf(stderr, "Serial: can't set %s to %u baud: %s\n", dev, baud, strerror(errno)); return false; }
    int lines = TIOCM_DTR | TIOCM_RTS;
    ioctl(fd, TIOCMBIS, &lines);
    ioctl(fd, TCFLSH, TCIOFLUSH);
    is_tty = true;
    printf("Serial: %s at %u baud, 8N1\n", dev, baud);
    return true;
}

static bool tcp_ready(int s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    fd = s;
    return true;
}

static bool open_tcp_listen(unsigned port) {
    int ls = socket(AF_INET6, SOCK_STREAM, 0);
    bool v6 = ls >= 0;
    if (!v6) ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("socket"); return false; }
    int one = 1, zero = 0;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    int r;
    if (v6) {
        setsockopt(ls, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
        sockaddr_in6 a;
        memset(&a, 0, sizeof a);
        a.sin6_family = AF_INET6;
        a.sin6_port = htons((uint16_t)port);
        a.sin6_addr = in6addr_any;
        r = bind(ls, (sockaddr*)&a, sizeof a);
    } else {
        sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        r = bind(ls, (sockaddr*)&a, sizeof a);
    }
    if (r != 0 || listen(ls, 1) != 0) { fprintf(stderr, "Serial: TCP port %u: %s\n", port, strerror(errno)); close(ls); return false; }
    printf("Serial: waiting for the other player on TCP port %u. Press Ctrl+C to exit.\n", port);
    for (;;) {
        struct pollfd p = {ls, POLLIN, 0};
        if (net_aborted()) { close(ls); return false; }
        if (poll(&p, 1, 200) > 0) {
            sockaddr_storage from;
            socklen_t fl = sizeof from;
            int s = accept(ls, (sockaddr*)&from, &fl);
            if (s < 0) continue;
            char h[INET6_ADDRSTRLEN] = "?";
            if (from.ss_family == AF_INET6) inet_ntop(AF_INET6, &((sockaddr_in6*)&from)->sin6_addr, h, sizeof h);
            else inet_ntop(AF_INET, &((sockaddr_in*)&from)->sin_addr, h, sizeof h);
            printf("Serial: connection from %s\n", h);
            close(ls);
            return tcp_ready(s);
        }
    }
}

static bool open_tcp_connect(const char* host, const char* port) {
    addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    int e = getaddrinfo(host, port, &hints, &res);
    if (e) { fprintf(stderr, "Serial: %s: %s\n", host, gai_strerror(e)); return false; }
    printf("Serial: connecting to %s:%s", host, port);
    fflush(stdout);
    // Keep trying, as one waits for the other end of a cable: whoever
    // starts first doesn't have to be the one listening... but someone is.
    for (int tries = 0; tries < 60 && !net_aborted(); tries++) {
        for (addrinfo* a = res; a; a = a->ai_next) {
            int s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s < 0) continue;
            if (connect(s, a->ai_addr, a->ai_addrlen) == 0) {
                freeaddrinfo(res);
                printf("\n");
                return tcp_ready(s);
            }
            close(s);
        }
        printf(".");
        fflush(stdout);
        usleep(1000 * 1000);
    }
    freeaddrinfo(res);
    printf("\nSerial: no connection\n");
    return false;
}

// ---------------------------------------------------------------- modem
static void ModemCommand(const char* str) {
    printf("Modem command : %s", str);
    for (const char* p = str; *p; p++) {
        write_buffer(p, 1);
        printf("%c", *p);
        fflush(stdout);
        usleep(100 * 1000);
    }
    write_buffer("\r", 1);
    printf("\n");
}

static bool ModemResponse(const char* resp) {
    char response[80];
    do {
        printf("Modem response: ");
        fflush(stdout);
        int rp = 0;
        for (;;) {
            if (net_aborted()) return false;
            int c = read_byte();
            if (c < 0) { wait_line(50); continue; }
            if (c == '\n' || rp == 79) { response[rp] = 0; printf("%s\n", response); break; }
            if (c >= ' ') response[rp++] = (char)c;
        }
    } while (strncmp(response, resp, strlen(resp)));
    return true;
}

static void ReadLine(FILE* f, char* dest, size_t n) {
    size_t k = 0;
    for (;;) {
        int c = fgetc(f);
        if (c == EOF || c == '\r' || c == '\n') break;
        if (k + 1 < n) dest[k++] = (char)c;
    }
    dest[k] = 0;
}

static unsigned ReadModemCfg(unsigned baud) {
    usemodem = true;
    FILE* f = fopen("modem.cfg", "r");
    if (!f) f = fopen("MODEM.CFG", "r");
    if (!f) { printf("Serial: no modem.cfg here; using ATZ\n"); return baud; }
    ReadLine(f, startup, sizeof startup);
    ReadLine(f, hangup, sizeof hangup);
    ReadLine(f, baudrate, sizeof baudrate);
    fclose(f);
    unsigned b = (unsigned)atol(baudrate);
    return b ? b : baud;
}

// ---------------------------------------------------------------- Connect
static bool Connect(int argc, char** argv, doomcom_t* dc) {
    printf("Attempting to connect across serial link, press Ctrl+C to abort.\n");
    // A (hopefully) unique id: SERSETUP hashes the clock and the interrupt
    // table; here the clock and the process.
    unsigned long idnum;
    if (net_arg(argc, argv, "-player1")) idnum = 0;
    else if (net_arg(argc, argv, "-player2")) idnum = 999999;
    else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        idnum = ((unsigned long)ts.tv_nsec / 1000 + (unsigned long)ts.tv_sec * 7919 + (unsigned long)getpid() * 104729) % 1000000;
    }
    char idstr[16], remoteidstr[16] = "";
    snprintf(idstr, sizeof idstr, "%06lu", idnum);

    // The packet is ID000000_0: the id, then the stage: 0, 1 once the other
    // side's id is known, 2 once the other side is at 1.
    int localstage = 0, remotestage = 0;
    (void)remotestage;
    time_t oldsec = -1;
    do {
        if (net_aborted()) return false;
        if (ReadPacket()) {
            packet[packetlen] = 0;
            printf("read : %s\n", (char*)packet);
            if (packetlen != 10 || strncmp((char*)packet, "ID", 2)) continue;
            if (!strncmp((char*)packet + 2, idstr, 6)) {
                printf("\n\nDuplicate id string, try again or check modem init string.\n");
                return false;
            }
            memcpy(remoteidstr, packet + 2, 6);
            remoteidstr[6] = 0;
            remotestage = packet[9] - '0';
            localstage = remotestage + 1;
            oldsec = -1;
        }
        time_t now = time(nullptr);
        if (now != oldsec) {
            oldsec = now;
            char str[40];
            snprintf(str, sizeof str, "ID%s_%i", idstr, localstage);
            WritePacket((const uint8_t*)str, (int)strlen(str));
            printf("wrote: %s\n", str);
        }
        wait_line(20);
    } while (localstage < 2);

    dc->consoleplayer = strcmp(remoteidstr, idstr) > 0 ? 0 : 1;
    // A last word for the other side, still at stage 1 maybe: a packet a
    // second goes on until it sees ours at 2 (SERSETUP's loop does the same).
    while (ReadPacket()) {}
    return true;
}

// ---------------------------------------------------------------- the driver
bool ser_wanted(int argc, char** argv) { return net_arg(argc, argv, "-serial") != 0; }

bool ser_setup(int argc, char** argv, doomcom_t* dc) {
    printf("---------------------------------\n"
           "DOOM SERIAL DEVICE DRIVER v1.4%s\n"
           "---------------------------------\n", net_driver_note);
    const char* where = net_arg_value(argc, argv, "-serial");
    if (!where) { printf("-serial: a device (/dev/ttyS0), HOST:PORT or listen:PORT\n"); return false; }

    unsigned baud = 14400;                          // SERSETUP's default
    bool modem = net_arg(argc, argv, "-dial") || net_arg(argc, argv, "-answer");
    if (modem) baud = ReadModemCfg(baud);
    static const unsigned rates[] = {9600, 14400, 19200, 38400, 57600, 115200};
    for (unsigned r : rates) {
        char opt[16];
        snprintf(opt, sizeof opt, "-%u", r);
        if (net_arg(argc, argv, opt)) baud = r;
    }

    bool ok;
    if (!strncasecmp(where, "listen:", 7)) ok = open_tcp_listen((unsigned)atoi(where + 7));
    else if (where[0] == '/') ok = open_tty(where, baud);
    else {
        std::string s = where;
        size_t c = s.rfind(':');
        if (c == std::string::npos) { printf("-serial %s: a device (/dev/...), HOST:PORT or listen:PORT\n", where); return false; }
        ok = open_tcp_connect(s.substr(0, c).c_str(), s.substr(c + 1).c_str());
    }
    if (!ok) return false;

    dc->ticdup = 1;
    dc->extratics = 0;
    dc->numnodes = 2;
    dc->numplayers = 2;
    dc->drone = 0;

    if (modem && !is_tty) printf("Serial: -dial / -answer need a serial device; ignored\n");
    if (modem && is_tty) {
        if (const char* num = net_arg_value(argc, argv, "-dial")) {
            ModemCommand(startup);
            if (!ModemResponse("OK")) return false;
            printf("\nDialing...\n\n");
            char cmd[96];
            snprintf(cmd, sizeof cmd, "ATDT%s", num);
            ModemCommand(cmd);
            if (!ModemResponse("CONNECT")) return false;
            dc->consoleplayer = 1;
        } else {
            ModemCommand(startup);
            if (!ModemResponse("OK")) return false;
            printf("\nWaiting for ring...\n\n");
            if (!ModemResponse("RING")) return false;
            ModemCommand("ATA");
            if (!ModemResponse("CONNECT")) return false;
            dc->consoleplayer = 0;
        }
    }
    if (!Connect(argc, argv, dc)) return false;
    printf("Console is player %i of 2\n", dc->consoleplayer + 1);
    return true;
}

// NetISR.
void ser_cmd(doomcom_t* dc) {
    if (dc->command == CMD_SEND) {
        WritePacket((const uint8_t*)&dc->data, dc->datalength);
    } else if (dc->command == CMD_GET) {
        if (ReadPacket() && packetlen <= NET_DATA_MAX) {
            dc->remotenode = 1;
            dc->datalength = (short)packetlen;
            memcpy(&dc->data, packet, packetlen);
        } else {
            dc->remotenode = -1;
        }
    }
}

void ser_shutdown() {
    if (fd < 0) return;
    // Let the last packets (Doom's goodbye) go out.
    uint32_t t0 = ms_now();
    while (!outque.empty() && ms_now() - t0 < 500) { flush_out(); if (!outque.empty()) wait_line(20); }
    if (usemodem && is_tty) {
        printf("\nDropping DTR\n");
        int dtr = TIOCM_DTR;
        ioctl(fd, TIOCMBIC, &dtr);
        usleep(1250 * 1000);
        ioctl(fd, TIOCMBIS, &dtr);
        ModemCommand("+++");
        usleep(1250 * 1000);
        ModemCommand(hangup);
        uint32_t t1 = ms_now();
        while (!outque.empty() && ms_now() - t1 < 2000) { flush_out(); wait_line(20); }
    }
    if (is_tty) {
        ioctl(fd, TCSBRK, 1);                       // drain
        if (tty_changed) ioctl(fd, TCSETS2, &tty_saved);
    }
    close(fd);
    fd = -1;
}

int ser_fds(int* fds) { fds[0] = fd; return fd >= 0; }
