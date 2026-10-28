// IPXSETUP.EXE, built in. The same packets as id's IPX driver (ipx/IPXNET.C,
// ipx/IPXSETUP.C, released under the GPL): IPX socket 869Bh (the IPXSETUP
// shipped with Doom 1.9; the released v1.22 source says 869Ch: -port 34460
// for that one), a 4-byte time
// stamp after the IPX header, the same setup broadcasts and the same way of
// numbering the players, so this plays against DOS machines running
// IPXSETUP and the original DOOM.EXE (or ../msdos's doom.exe).
//
// Linux has no IPX any more, so the packets go one of these ways:
//
//   -ipx [-udpport N]      IPX over UDP broadcast on the LAN, Linux to Linux,
//                          no server (each machine's node: its IPv4 address
//                          and UDP port, as DOSBox's)
//   -ipxnet HOST[:PORT]    a DOSBox IPX tunnel server (DOSBox's "ipxnet
//                          startserver", ipxbox, ipxgw...): play with DOS
//                          Doom in DOSBox anywhere, or with real DOS
//                          machines bridged by ipxbox / ipxgw
//   -ipxserver [PORT]      be that server (and play): DOSBox machines then
//                          "ipxnet connect" here
//   -ipxeth IFACE          real IPX frames on an Ethernet interface, for DOS
//                          PCs on the LAN running a packet driver or ODI IPX
//                          (needs CAP_NET_RAW). -ipxframe ii|802.3|802.2|snap
//
// and as IPXSETUP: -nodes N (players, default 2), -port N (the IPX socket,
// decimal; default 34459 = 869Bh, as Doom 1.9's IPXSETUP). The game options
// (-deathmatch, -warp, -skill...) are Doom's own, as with IPXSETUP.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "net_linux.h"

// ---------------------------------------------------------------- packets
// The IPX header (all big-endian), 30 bytes; then Doom's driver adds a
// little-endian long (the sender's packet count, -1 in setup) and the data,
// and sends 4 bytes more than that (IPXNET.C: f2Size = datalength + 4),
// which is why the receiver takes 38 off the IPX length.
enum {
    H_CHECKSUM = 0, H_LENGTH = 2, H_TC = 4, H_TYPE = 5,
    H_DNET = 6, H_DNODE = 10, H_DSOCK = 16,
    H_SNET = 18, H_SNODE = 22, H_SSOCK = 28,
    IPX_HDR = 30, IPX_TIME = 30, IPX_DATA = 34, IPX_SLOP = 4,
    IPX_MAX = 576,
};
static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static uint32_t ms_now() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void wait_fd(int fd, int ms) {
    struct pollfd p = {fd, POLLIN, 0};
    poll(&p, 1, ms);
}

static void nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

static const char* node_str(const uint8_t* n) {
    static char s[4][24];
    static int k;
    k = (k + 1) & 3;
    snprintf(s[k], sizeof s[k], "%02X%02X%02X%02X%02X%02X", n[0], n[1], n[2], n[3], n[4], n[5]);
    return s[k];
}

// ---------------------------------------------------------------- links
// A way to move whole IPX packets (header included). The destination is in
// the header.
struct Link {
    uint8_t net[4] = {0, 0, 0, 0};
    uint8_t node[6] = {0, 0, 0, 0, 0, 0};
    int fd = -1;
    virtual ~Link() { if (fd >= 0) close(fd); }
    virtual bool open() = 0;
    virtual void send(const uint8_t* pkt, int len) = 0;
    virtual int recv(uint8_t* buf, int max) = 0;     // bytes, or 0: none waiting
    virtual void service() {}                       // between packets (the built-in server)
    virtual int fds(int* out) { out[0] = fd; return fd >= 0; }
};

static bool udp_addr(const char* host, uint16_t port, sockaddr_in* sa) {
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_port = htons(port);
    if (inet_pton(AF_INET, host, &sa->sin_addr) == 1) return true;
    addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) return false;
    sa->sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

// ---- IPX over UDP broadcast on the LAN. A node is an IPv4 address and a
// UDP port (network order), as in DOSBox's tunnel; a packet to node
// FFFFFFFFFFFF goes to the interface's broadcast address.
struct UdpLan : Link {
    uint16_t port;
    const char* want_ip;
    sockaddr_in bcast;
    UdpLan(uint16_t p, const char* ip) : port(p), want_ip(ip) {}

    bool open() override {
        // The interface: the one with -ipxbind's address, else the first
        // that is up, not loopback, and can broadcast.
        ifaddrs* ifs = nullptr;
        if (getifaddrs(&ifs) != 0) { perror("getifaddrs"); return false; }
        in_addr me{}, bc{};
        const char* ifname = nullptr;
        in_addr want{};
        if (want_ip && inet_pton(AF_INET, want_ip, &want) != 1) {
            fprintf(stderr, "IPX: -ipxbind %s isn't an IPv4 address\n", want_ip);
            freeifaddrs(ifs);
            return false;
        }
        for (ifaddrs* i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
            in_addr a = ((sockaddr_in*)i->ifa_addr)->sin_addr;
            if (want_ip ? a.s_addr != want.s_addr : !(i->ifa_flags & IFF_BROADCAST) || !i->ifa_broadaddr) continue;
            me = a;
            bc = (i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr ? ((sockaddr_in*)i->ifa_broadaddr)->sin_addr
                                                                     : in_addr{htonl(INADDR_BROADCAST)};
            ifname = i->ifa_name;
            break;
        }
        if (!ifname) {
            fprintf(stderr, "IPX: no network interface to broadcast on%s\n", want_ip ? " with that address" : "");
            freeifaddrs(ifs);
            return false;
        }
        char a1[32], a2[32];
        inet_ntop(AF_INET, &me, a1, sizeof a1);
        inet_ntop(AF_INET, &bc, a2, sizeof a2);
        printf("IPX: over UDP on %s (%s), broadcast %s, port %u\n", ifname, a1, a2, port);
        freeifaddrs(ifs);

        fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) { perror("socket"); return false; }
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr.s_addr = htonl(INADDR_ANY);           // broadcasts arrive at INADDR_ANY only
        if (bind(fd, (sockaddr*)&sa, sizeof sa) != 0) {
            fprintf(stderr, "IPX: UDP port %u: %s%s\n", port, strerror(errno),
                    errno == EADDRINUSE ? " (another Doom here? -udpport N, the same on every machine)" : "");
            return false;
        }
        nonblock(fd);
        memset(&bcast, 0, sizeof bcast);
        bcast.sin_family = AF_INET;
        bcast.sin_port = htons(port);
        bcast.sin_addr = bc;
        memcpy(node, &me.s_addr, 4);
        node[4] = (uint8_t)(port >> 8); node[5] = (uint8_t)port;
        return true;
    }

    void send(const uint8_t* pkt, int len) override {
        const uint8_t* d = pkt + H_DNODE;
        sockaddr_in to;
        if (!memcmp(d, BROADCAST, 6)) to = bcast;
        else {
            memset(&to, 0, sizeof to);
            to.sin_family = AF_INET;
            memcpy(&to.sin_addr.s_addr, d, 4);
            to.sin_port = htons(be16(d + 4));
        }
        sendto(fd, pkt, len, 0, (sockaddr*)&to, sizeof to);
    }

    int recv(uint8_t* buf, int max) override {
        for (;;) {
            ssize_t n = ::recv(fd, buf, max, 0);
            if (n < 0) return 0;
            if (n < IPX_HDR) continue;
            if (!memcmp(buf + H_SNODE, node, 6)) continue;    // our own broadcast, back
            return (int)n;
        }
    }
};

// ---- A DOSBox IPX tunnel server. Each IPX packet goes whole in a UDP
// datagram to the server, which forwards it by its destination node (an
// IPv4 address and port: how the server sees each client). Joining: an
// "echo" packet (socket 2) with a zero destination; the server answers to
// socket 2 with our node and network in the destination fields. Wire format
// as DOSBox's ipx.cpp / ipxserver.cpp.
static void reg_header(uint8_t* h) {
    memset(h, 0, IPX_HDR);
    put16(h + H_CHECKSUM, 0xFFFF);
    put16(h + H_LENGTH, IPX_HDR);
    put16(h + H_DSOCK, 2);
    put16(h + H_SSOCK, 2);
}

struct TunnelClient : Link {
    sockaddr_in server;
    char where[300];
    TunnelClient(const sockaddr_in& s, const char* w) : server(s) { snprintf(where, sizeof where, "%s", w); }

    bool open() override {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) { perror("socket"); return false; }
        if (connect(fd, (sockaddr*)&server, sizeof server) != 0) {
            fprintf(stderr, "IPX: %s: %s\n", where, strerror(errno));
            return false;
        }
        nonblock(fd);
        uint8_t reg[IPX_HDR], in[IPX_MAX];
        reg_header(reg);
        printf("IPX: joining the IPX tunnel server at %s", where);
        fflush(stdout);
        for (int tries = 0; tries < 10 && !net_aborted(); tries++) {
            ::send(fd, reg, sizeof reg, 0);
            uint32_t t0 = ms_now();
            while (ms_now() - t0 < 1000 && !net_aborted()) {
                net_tick(-1, 0);
                wait_fd(fd, 50);
                ssize_t n = ::recv(fd, in, sizeof in, 0);
                if (n >= IPX_HDR && be16(in + H_DSOCK) == 2 && be16(in + H_SSOCK) == 2) {
                    memcpy(net, in + H_DNET, 4);
                    memcpy(node, in + H_DNODE, 6);
                    printf("\nIPX: joined; our node %s, network %02X%02X%02X%02X\n", node_str(node),
                           net[0], net[1], net[2], net[3]);
                    return true;
                }
            }
            printf(".");
            fflush(stdout);
        }
        printf("\nIPX: no answer from %s (is the server running? the UDP port open?)\n", where);
        return false;
    }

    void send(const uint8_t* pkt, int len) override { ::send(fd, pkt, len, 0); }

    int recv(uint8_t* buf, int max) override {
        for (;;) {
            ssize_t n = ::recv(fd, buf, max, 0);
            if (n < 0) return 0;
            if (n < IPX_HDR) continue;
            if (be16(buf + H_DSOCK) == 2) {
                // A ping (DOSBox's "ipxnet ping", ipxbox's keepalives):
                // broadcast to socket 2. DOSBox answers it; so do we.
                if (!memcmp(buf + H_DNODE, BROADCAST, 6)) {
                    uint8_t r[IPX_HDR];
                    reg_header(r);
                    memcpy(r + H_DNET, buf + H_SNET, 4);
                    memcpy(r + H_DNODE, buf + H_SNODE, 6);
                    memcpy(r + H_SNET, net, 4);
                    memcpy(r + H_SNODE, node, 6);
                    ::send(fd, r, sizeof r, 0);
                }
                continue;
            }
            return (int)n;
        }
    }
};

// The server itself, as DOSBox's ipxserver.cpp: up to 64 clients, each
// known by the address its datagrams come from.
struct TunnelServer {
    int fd = -1;
    uint16_t port;
    struct Client { sockaddr_in a; };
    Client clients[64];
    int nclients = 0;

    ~TunnelServer() { if (fd >= 0) close(fd); }

    bool open(uint16_t p) {
        port = p;
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) { perror("socket"); return false; }
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(fd, (sockaddr*)&sa, sizeof sa) != 0) {
            fprintf(stderr, "IPX: server on UDP port %u: %s\n", port, strerror(errno));
            if (errno == EACCES)
                fprintf(stderr, "     Ports below 1024 need root. Use another (-ipxserver 21300; in DOSBox:\n"
                                "     ipxnet connect <this machine> 21300), or\n"
                                "     sudo setcap cap_net_bind_service+ep ./doom\n");
            return false;
        }
        nonblock(fd);
        printf("IPX: tunnel server on UDP port %u (DOSBox: ipxnet connect <this machine> %u)\n", port, port);
        return true;
    }

    static void pack(uint8_t* n, const sockaddr_in& a) {
        memcpy(n, &a.sin_addr.s_addr, 4);
        memcpy(n + 4, &a.sin_port, 2);
    }

    void ack(const sockaddr_in& to) {
        uint8_t h[IPX_HDR];
        reg_header(h);
        pack(h + H_DNODE, to);
        h[H_SNET + 3] = 1;                              // as DOSBox: network 1 in the source
        sockaddr_in me;
        socklen_t ml = sizeof me;
        getsockname(fd, (sockaddr*)&me, &ml);
        pack(h + H_SNODE, me);
        sendto(fd, h, sizeof h, 0, (const sockaddr*)&to, sizeof to);
    }

    void pump() {
        uint8_t buf[2048];
        for (;;) {
            sockaddr_in from;
            socklen_t fl = sizeof from;
            ssize_t n = recvfrom(fd, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
            if (n < 0) return;
            if (n < IPX_HDR) continue;
            // Joining: an echo packet to a zero node.
            if (be16(buf + H_DSOCK) == 2 && !memcmp(buf + H_DNODE, "\0\0\0\0", 4)) {
                int i;
                for (i = 0; i < nclients; i++)
                    if (clients[i].a.sin_addr.s_addr == from.sin_addr.s_addr && clients[i].a.sin_port == from.sin_port)
                        break;
                if (i == nclients) {
                    if (nclients == 64) continue;
                    clients[nclients++].a = from;
                    char s[32];
                    inet_ntop(AF_INET, &from.sin_addr, s, sizeof s);
                    printf("IPX server: %s:%u joined\n", s, ntohs(from.sin_port));
                }
                ack(from);
                continue;
            }
            // Forward by the destination node: everyone else for a broadcast.
            const uint8_t* d = buf + H_DNODE;
            const uint8_t* s = buf + H_SNODE;
            bool bc = !memcmp(d, BROADCAST, 4);
            for (int i = 0; i < nclients; i++) {
                uint8_t cn[6];
                pack(cn, clients[i].a);
                if (bc ? !memcmp(cn, s, 6) : memcmp(cn, d, 6)) continue;
                sendto(fd, buf, n, 0, (const sockaddr*)&clients[i].a, sizeof clients[i].a);
                if (!bc) break;
            }
        }
    }
};

// -ipxserver: the server, and this game joined to it over the loopback.
struct ServerAndClient : TunnelClient {
    TunnelServer* srv;
    ServerAndClient(TunnelServer* s, const sockaddr_in& a, const char* w) : TunnelClient(a, w), srv(s) {}
    ~ServerAndClient() override { delete srv; }
    bool open() override {
        // Answer our own join while TunnelClient::open waits for it.
        fd = -1;
        sockaddr_in a = server;
        int f = socket(AF_INET, SOCK_DGRAM, 0);
        if (f < 0) return false;
        connect(f, (sockaddr*)&a, sizeof a);
        nonblock(f);
        uint8_t reg[IPX_HDR], in[IPX_MAX];
        reg_header(reg);
        for (int tries = 0; tries < 20; tries++) {
            ::send(f, reg, sizeof reg, 0);
            for (int k = 0; k < 10; k++) {
                srv->pump();
                wait_fd(f, 10);
                ssize_t n = ::recv(f, in, sizeof in, 0);
                if (n >= IPX_HDR && be16(in + H_DSOCK) == 2) {
                    memcpy(net, in + H_DNET, 4);
                    memcpy(node, in + H_DNODE, 6);
                    fd = f;
                    return true;
                }
            }
        }
        close(f);
        fprintf(stderr, "IPX: couldn't join our own server\n");
        return false;
    }
    void service() override { srv->pump(); }
    int fds(int* out) override { int n = TunnelClient::fds(out); out[n++] = srv->fd; return n; }
    void send(const uint8_t* pkt, int len) override { TunnelClient::send(pkt, len); srv->pump(); }
    int recv(uint8_t* buf, int max) override { srv->pump(); return TunnelClient::recv(buf, max); }
};

// ---- Real IPX on Ethernet (AF_PACKET). The node is the card's MAC
// address. Frames come in any of the four kinds IPX uses; ours go out as
// -ipxframe says: Ethernet II (type 8137h, the default; what most DOS
// packet-driver setups and DOSBox bridges use), raw 802.3 (Novell's old
// default), 802.2 LLC (NetWare 3.12+'s default) or SNAP.
enum Frame { F_II, F_8023, F_8022, F_SNAP };

struct RawEth : Link {
    const char* ifname;
    Frame frame;
    int ifindex = 0;
    RawEth(const char* i, Frame f) : ifname(i), frame(f) {}

    bool open() override {
        fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
        if (fd < 0) {
            fprintf(stderr, "IPX: raw Ethernet: %s\n", strerror(errno));
            if (errno == EPERM || errno == EACCES)
                fprintf(stderr, "     Needs CAP_NET_RAW: sudo setcap cap_net_raw+ep ./doom (or run as root)\n");
            return false;
        }
        ifreq ifr;
        memset(&ifr, 0, sizeof ifr);
        snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifname);
        if (ioctl(fd, SIOCGIFINDEX, &ifr) != 0) { fprintf(stderr, "IPX: no interface %s\n", ifname); return false; }
        ifindex = ifr.ifr_ifindex;
        if (ioctl(fd, SIOCGIFHWADDR, &ifr) != 0) { perror("SIOCGIFHWADDR"); return false; }
        memcpy(node, ifr.ifr_hwaddr.sa_data, 6);
        sockaddr_ll sll;
        memset(&sll, 0, sizeof sll);
        sll.sll_family = AF_PACKET;
        sll.sll_protocol = htons(ETH_P_ALL);
        sll.sll_ifindex = ifindex;
        if (bind(fd, (sockaddr*)&sll, sizeof sll) != 0) { perror("bind"); return false; }
        nonblock(fd);
        static const char* names[] = {"Ethernet II", "raw 802.3", "802.2", "SNAP"};
        printf("IPX: Ethernet on %s, node %s, %s frames\n", ifname, node_str(node), names[frame]);
        return true;
    }

    void send(const uint8_t* pkt, int len) override {
        uint8_t f[1600];
        memcpy(f, pkt + H_DNODE, 6);                    // the IPX node is the MAC address
        memcpy(f + 6, node, 6);
        int p = 14;
        switch (frame) {
        case F_II: put16(f + 12, 0x8137); break;
        case F_8023: put16(f + 12, (uint16_t)len); break;
        case F_8022: put16(f + 12, (uint16_t)(len + 3)); f[p++] = 0xE0; f[p++] = 0xE0; f[p++] = 0x03; break;
        case F_SNAP:
            put16(f + 12, (uint16_t)(len + 8));
            f[p++] = 0xAA; f[p++] = 0xAA; f[p++] = 0x03; f[p++] = 0; f[p++] = 0; f[p++] = 0;
            put16(f + p, 0x8137); p += 2;
            break;
        }
        memcpy(f + p, pkt, len);
        p += len;
        while (p < 60) f[p++] = 0;                      // the shortest Ethernet frame
        sockaddr_ll to;
        memset(&to, 0, sizeof to);
        to.sll_family = AF_PACKET;
        to.sll_ifindex = ifindex;
        to.sll_halen = 6;
        memcpy(to.sll_addr, f, 6);
        sendto(fd, f, p, 0, (sockaddr*)&to, sizeof to);
    }

    int recv(uint8_t* buf, int max) override {
        uint8_t f[2048];
        for (;;) {
            sockaddr_ll from;
            socklen_t fl = sizeof from;
            ssize_t n = recvfrom(fd, f, sizeof f, 0, (sockaddr*)&from, &fl);
            if (n < 0) return 0;
            if (from.sll_pkttype == PACKET_OUTGOING || n < 14 + IPX_HDR) continue;
            uint16_t type = be16(f + 12);
            int p;
            if (type == 0x8137) p = 14;
            else if (type <= 1500) {
                if (f[14] == 0xFF && f[15] == 0xFF) p = 14;                 // raw 802.3: the IPX checksum
                else if (f[14] == 0xE0 && f[15] == 0xE0 && f[16] == 0x03) p = 17;
                else if (f[14] == 0xAA && f[15] == 0xAA && f[16] == 0x03 && be16(f + 20) == 0x8137) p = 22;
                else continue;
            } else continue;
            int len = (int)n - p;
            if (len < IPX_HDR) continue;
            int il = be16(f + p + H_LENGTH);
            if (il >= IPX_HDR && il < len) len = il;    // without the Ethernet padding
            if (len > max) len = max;
            memcpy(buf, f + p, len);
            if (!memcmp(buf + H_SNODE, node, 6)) continue;
            return len;
        }
    }
};

// ---------------------------------------------------------------- the driver
// IPXNET.C's state. nodeadr[0] is this machine, nodeadr[MAXNETNODES] the
// broadcast address.
static Link* link_;
static uint16_t socketid = 0x869B;     // Doom 1.9's IPXSETUP.EXE (seen on the wire)
static uint8_t nodeadr[MAXNETNODES + 1][6];
static int32_t localtime_;                   // packets sent; -1 in setup
static int32_t remotetime;

// IPXNET.C has ten receive buffers and hands back the one with the lowest
// time stamp; here a queue, the same order.
struct Rx {
    int32_t time;
    uint8_t node[6];
    int len;
    uint8_t data[IPX_MAX];
};
static Rx rxq[64];
static int rxn;

static void drain() {
    if (!link_) return;
    link_->service();
    uint8_t buf[2048];
    for (;;) {
        int n = link_->recv(buf, sizeof buf);
        if (n <= 0) break;
        if (be16(buf + H_DSOCK) != socketid) continue;
        int il = be16(buf + H_LENGTH);
        if (il > n) il = n;                         // a short datagram: take what came
        if (il < IPX_DATA + IPX_SLOP) continue;
        if (rxn == (int)(sizeof rxq / sizeof rxq[0])) continue;   // full: lost, as with no free ECB
        Rx& r = rxq[rxn++];
        r.time = (int32_t)(buf[IPX_TIME] | buf[IPX_TIME + 1] << 8 | buf[IPX_TIME + 2] << 16 | (uint32_t)buf[IPX_TIME + 3] << 24);
        memcpy(r.node, buf + H_SNODE, 6);
        r.len = il - IPX_DATA - IPX_SLOP;           // ShortSwap(PacketLength) - 38
        if (r.len > (int)sizeof r.data) r.len = sizeof r.data;
        int have = n - IPX_DATA;
        memset(r.data, 0, sizeof r.data);
        memcpy(r.data, buf + IPX_DATA, have < r.len ? have : r.len);
    }
}

static void send_packet(doomcom_t* dc, int dest) {
    uint8_t p[IPX_MAX];
    int dl = dc->datalength;
    if (dl < 0) dl = 0;
    if (dl > NET_DATA_MAX) dl = NET_DATA_MAX;
    int len = IPX_DATA + dl + IPX_SLOP;
    memset(p, 0, len);
    put16(p + H_CHECKSUM, 0xFFFF);
    put16(p + H_LENGTH, (uint16_t)len);
    p[H_TYPE] = 4;                                  // packet exchange
    memcpy(p + H_DNET, link_->net, 4);
    memcpy(p + H_DNODE, nodeadr[dest], 6);
    put16(p + H_DSOCK, socketid);
    memcpy(p + H_SNET, link_->net, 4);
    memcpy(p + H_SNODE, link_->node, 6);
    put16(p + H_SSOCK, socketid);
    uint32_t t = (uint32_t)localtime_;
    p[IPX_TIME] = (uint8_t)t; p[IPX_TIME + 1] = (uint8_t)(t >> 8);
    p[IPX_TIME + 2] = (uint8_t)(t >> 16); p[IPX_TIME + 3] = (uint8_t)(t >> 24);
    memcpy(p + IPX_DATA, &dc->data, dl);
    link_->send(p, len);
}


// GetPacket: true with a packet in dc; remotenode -1 for one from a node
// not known yet (in setup), its address in remoteadr.
static uint8_t remoteadr[6];

static bool get_packet(doomcom_t* dc) {
    dc->remotenode = -1;
    drain();
    while (rxn) {
        int best = 0;
        for (int i = 1; i < rxn; i++) if (rxq[i].time < rxq[best].time) best = i;
        Rx r = rxq[best];
        rxq[best] = rxq[--rxn];
        if (r.time == -1 && localtime_ != -1) continue;    // a setup broadcast, game on
        int i;
        for (i = 0; i < dc->numnodes; i++) if (!memcmp(r.node, nodeadr[i], 6)) break;
        if (i < dc->numnodes) dc->remotenode = (short)i;
        else if (localtime_ != -1) continue;            // not one of ours
        memcpy(remoteadr, r.node, 6);
        remotetime = r.time;
        dc->datalength = (short)r.len;
        memcpy(&dc->data, r.data, r.len < NET_DATA_MAX ? r.len : NET_DATA_MAX);
        return true;
    }
    return false;
}

// IPXNET.H: during setup, doomdata is this.
struct setupdata_t {
    int16_t gameid;
    int16_t drone;
    int16_t nodesfound;
    int16_t nodeswanted;
};

// IPXSETUP.C's LookForNodes: broadcast every second how many nodes we've
// found, until every node has found all of them. Then the players are
// numbered by node address, lowest first, so every machine agrees.
static bool look_for_nodes(doomcom_t* dc, int numnetnodes) {
    setupdata_t nodesetup[MAXNETNODES];
    memset(nodesetup, 0, sizeof nodesetup);
    setupdata_t* setup = (setupdata_t*)&dc->data;

    printf("Attempting to find all players for %i player net play. Press Ctrl+C to exit.\n", numnetnodes);
    printf("Looking for a node");
    fflush(stdout);
    localtime_ = -1;
    nodesetup[0].nodesfound = 1;
    nodesetup[0].nodeswanted = (int16_t)numnetnodes;
    dc->numnodes = 1;
    time_t oldsec = -1;

    for (;;) {
        net_tick(dc->numnodes, numnetnodes);
        if (net_aborted()) return false;
        while (get_packet(dc)) {
            setupdata_t* dest = dc->remotenode == -1 ? &nodesetup[dc->numnodes] : &nodesetup[dc->remotenode];
            if (remotetime != -1) {
                // An early game packet: that node has found everyone.
                if (dc->remotenode == -1) {
                    printf("\nGot an unknown game packet during setup\n");
                    return false;
                }
                dest->nodesfound = dest->nodeswanted;
                continue;
            }
            memcpy(dest, setup, sizeof *dest);
            if (dc->remotenode != -1) continue;         // a node we know
            if (dc->numnodes >= MAXNETNODES) {
                printf("\nMore than %d nodes are looking for a game\n", MAXNETNODES);
                return false;
            }
            memcpy(nodeadr[dc->numnodes], remoteadr, 6);
            dc->numnodes++;
            printf("\nFound a node! (%s)\n", node_str(remoteadr));
            if (dc->numnodes < numnetnodes) printf("Looking for a node");
            fflush(stdout);
        }

        int i;
        for (i = 0; i < dc->numnodes; i++)
            if (nodesetup[i].nodesfound != nodesetup[i].nodeswanted) break;
        if (i == nodesetup[0].nodeswanted) break;     // got them all

        time_t now = time(nullptr);
        if (now != oldsec) {
            oldsec = now;
            printf(".");
            fflush(stdout);
            dc->datalength = sizeof(setupdata_t);
            nodesetup[0].nodesfound = (int16_t)dc->numnodes;
            memcpy(&dc->data, &nodesetup[0], sizeof(setupdata_t));
            send_packet(dc, MAXNETNODES);               // to all
        }
        if (link_->fd >= 0) wait_fd(link_->fd, 20);
    }

    // Number the players: one more than the nodes with a lower address.
    int total = 0, console = 0;
    for (int i = 0; i < numnetnodes; i++) {
        if (nodesetup[i].drone) continue;
        total++;
        if (total > 4) { printf("More than %i players specified!\n", 4); return false; }
        if (memcmp(nodeadr[i], nodeadr[0], 6) < 0) console++;
    }
    if (!total) { printf("No players specified for game!\n"); return false; }
    dc->consoleplayer = (short)console;
    dc->numplayers = (short)total;
    printf("Console is player %i of %i\n", console + 1, total);
    return true;
}

bool ipx_wanted(int argc, char** argv) {
    return net_arg(argc, argv, "-ipx") || net_arg(argc, argv, "-ipxnet") || net_arg(argc, argv, "-ipxserver")
        || net_arg(argc, argv, "-ipxeth");
}

// A port after the name, if there is one (not the next option).
static uint16_t port_arg(int argc, char** argv, const char* name, uint16_t def) {
    const char* v = net_arg_value(argc, argv, name);
    if (!v || v[0] < '0' || v[0] > '9') return def;
    int p = atoi(v);
    return p > 0 && p < 65536 ? (uint16_t)p : def;
}

bool ipx_setup(int argc, char** argv, doomcom_t* dc) {
    printf("-----------------------------\n"
           "DOOM NETWORK DEVICE DRIVER\n"
           "IPXSETUP v1.22%s\n"
           "-----------------------------\n", net_driver_note);
    int numnetnodes = 2;
    if (const char* v = net_arg_value(argc, argv, "-nodes")) numnetnodes = atoi(v);
    if (numnetnodes < 1 || numnetnodes > MAXNETNODES) {
        printf("-nodes: 1 to %d\n", MAXNETNODES);
        return false;
    }
    if (const char* v = net_arg_value(argc, argv, "-port")) {
        socketid = (uint16_t)atoi(v);
        printf("Using alternate port %i for network\n", socketid);
    }

    if (const char* v = net_arg_value(argc, argv, "-ipxnet")) {
        // HOST, HOST:PORT or HOST PORT (as DOSBox's ipxnet connect)
        char host[256];
        snprintf(host, sizeof host, "%s", v);
        uint16_t port = 213;
        if (char* c = strrchr(host, ':')) { *c = 0; port = (uint16_t)atoi(c + 1); }
        else port = port_arg(argc, argv, v, 213);
        sockaddr_in sa;
        if (!udp_addr(host, port, &sa)) { printf("IPX: can't find %s\n", host); return false; }
        char where[300];
        snprintf(where, sizeof where, "%s:%u", host, port);
        link_ = new TunnelClient(sa, where);
    } else if (net_arg(argc, argv, "-ipxserver")) {
        uint16_t port = port_arg(argc, argv, "-ipxserver", 213);
        TunnelServer* srv = new TunnelServer;
        if (!srv->open(port)) { delete srv; return false; }
        sockaddr_in sa;
        udp_addr("127.0.0.1", port, &sa);
        link_ = new ServerAndClient(srv, sa, "this machine");
    } else if (const char* v = net_arg_value(argc, argv, "-ipxeth")) {
        Frame f = F_II;
        if (const char* t = net_arg_value(argc, argv, "-ipxframe")) {
            if (!strcasecmp(t, "802.3") || !strcasecmp(t, "raw")) f = F_8023;
            else if (!strcasecmp(t, "802.2") || !strcasecmp(t, "llc")) f = F_8022;
            else if (!strcasecmp(t, "snap")) f = F_SNAP;
            else if (!strcasecmp(t, "ii") || !strcasecmp(t, "ethernet_ii")) f = F_II;
            else { printf("-ipxframe: ii, 802.3, 802.2 or snap\n"); return false; }
        }
        link_ = new RawEth(v, f);
    } else {
        link_ = new UdpLan(port_arg(argc, argv, "-udpport", 10213), net_arg_value(argc, argv, "-ipxbind"));
    }
    if (!link_->open()) return false;

    memcpy(nodeadr[0], link_->node, 6);
    memset(nodeadr[MAXNETNODES], 0xFF, 6);
    dc->extratics = 1;                              // as IPXSETUP
    if (!look_for_nodes(dc, numnetnodes)) return false;
    localtime_ = 0;
    return true;
}

// NetISR.
void ipx_cmd(doomcom_t* dc) {
    if (dc->command == CMD_SEND) {
        localtime_++;
        int d = dc->remotenode;
        if (d >= 0 && d < dc->numnodes) send_packet(dc, d);
    } else if (dc->command == CMD_GET) {
        get_packet(dc);
    }
}

void ipx_shutdown() {
    // A last chance for the server to pass on our goodbye packets.
    if (link_) { link_->service(); delete link_; link_ = nullptr; }
}

int ipx_fds(int* fds) { return link_ ? link_->fds(fds) : 0; }
