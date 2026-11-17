// netrole.h - the network role rule behind API 2.26 net_role. Pure code, no engine calls.
// Same rule as the telemetry's role_name (lan_telemetry.c): a joiner keeps the world (and the authority game mode) it had in single player, so the
// server connection (a client net driver has one, a listen server never does) or the loader's joiner test decides "client" first.
#ifndef LANNATIVE_NETROLE_H
#define LANNATIVE_NETROLE_H
enum { NR_NONE = -1, NR_STANDALONE = 0, NR_HOST = 1, NR_CLIENT = 2 };
// world: a world exists; netdriver: it has a (valid) net driver; server_conn: that driver has a server connection; joiner: the loader's joiner test;
// auth_gm: the world has an authority game mode. Solo with the LAN mod off (default) has no net driver: NR_STANDALONE; a LAN host has one: NR_HOST.
static int nr_role(int world, int netdriver, int server_conn, int joiner, int auth_gm) {
    if (!world) return NR_NONE;
    if (server_conn || (netdriver && joiner)) return NR_CLIENT;
    if (auth_gm) return netdriver ? NR_HOST : NR_STANDALONE;
    return NR_NONE;
}
static const char *nr_name(int r) { return r == NR_STANDALONE ? "standalone" : r == NR_HOST ? "host" : r == NR_CLIENT ? "client" : "none"; }
#endif
