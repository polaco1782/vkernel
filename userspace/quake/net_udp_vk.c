/*
 * net_udp_vk.c - UDP network stub for vkernel (no networking)
 * Replaces src/net/src/net_udp.c
 */

#include "net_udp.h"
#include "sys.h"

qboolean UDP_IsInitialized(void) { return false; }
UDPsocket UDP_GetControlSocket(void) { return NULL; }
UDPsocket UDP_GetAcceptSocket(void)  { return NULL; }
void UDP_Init(void)              {}
void UDP_Shutdown(void)          {}
void UDP_Listen(qboolean state)  { (void)state; }

UDPsocket UDP_OpenSocket(i32 port)
{
    (void)port;
    return NULL;
}

void UDP_CloseSocket(UDPsocket socket) { (void)socket; }

i32 UDP_Read(UDPsocket socket, byte *buf, i32 len, IPaddress *addr)
{
    (void)socket; (void)buf; (void)len; (void)addr;
    return -1;
}

i32 UDP_Write(UDPsocket socket, byte *buf, i32 len, const IPaddress *addr)
{
    (void)socket; (void)buf; (void)len; (void)addr;
    return -1;
}

i32 UDP_Broadcast(UDPsocket socket, byte *buf, i32 len)
{
    (void)socket; (void)buf; (void)len;
    return -1;
}

char *UDP_AddrToString(const IPaddress *addr)
{
    (void)addr;
    return "0.0.0.0:0";
}

IPaddress UDP_GetSocketAddr(UDPsocket socket)
{
    (void)socket;
    IPaddress a = {0, 0};
    return a;
}

void UDP_GetNameFromAddr(const IPaddress *addr, char *name)
{
    (void)addr;
    if (name) name[0] = '\0';
}

i32 UDP_GetAddrFromName(char *name, IPaddress *addr)
{
    (void)name; (void)addr;
    return -1;
}

i32 UDP_AddrCompare(const IPaddress *addr1, const IPaddress *addr2)
{
    if (!addr1 || !addr2) return -1;
    if (addr1->host != addr2->host) return -1;
    if (addr1->port != addr2->port) return -1;
    return 0;
}

i32 UDP_GetSocketPort(const IPaddress *addr)
{
    return addr ? (i32)addr->port : 0;
}

void UDP_SetSocketPort(IPaddress *addr, i32 port)
{
    if (addr) addr->port = (Uint16)port;
}
