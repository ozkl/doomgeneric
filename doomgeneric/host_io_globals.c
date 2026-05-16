// host_io_globals.c — Minimal stubs for host_io.h
//
// NOTE: host_io.h is automatically force-included via -include flag.

int host_feof_stub(FILE *f) {
    (void)f;
    return 0;  // stub — not used in active doom code paths
}
