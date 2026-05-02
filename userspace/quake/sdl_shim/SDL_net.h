/*
 * SDL_net.h - Stub for vkernel (no real network support)
 * UDPsocket/IPaddress types needed by net headers.
 */
#ifndef VK_SDL_NET_H
#define VK_SDL_NET_H

#include "SDL.h"
#include <stdint.h>

typedef struct {
    Uint32 host;
    Uint16 port;
} IPaddress;

typedef void* UDPsocket;
typedef void* TCPsocket;

typedef struct {
    int channel;
    Uint8 *data;
    int len;
    int maxlen;
    int status;
    IPaddress address;
} UDPpacket;

#define INADDR_NONE      0xFFFFFFFFu
#define INADDR_ANY       0x00000000u
#define INADDR_BROADCAST 0xFFFFFFFFu

static inline int    SDLNet_Init(void)                             { return -1; }
static inline void   SDLNet_Quit(void)                             {}
static inline int    SDLNet_ResolveHost(IPaddress *a, const char *h, Uint16 p)
                                                                   { (void)a;(void)h;(void)p; return -1; }
static inline const char *SDLNet_ResolveIP(const IPaddress *a)    { (void)a; return ""; }
static inline UDPsocket SDLNet_UDP_Open(Uint16 p)                 { (void)p; return NULL; }
static inline void   SDLNet_UDP_Close(UDPsocket s)                { (void)s; }
static inline int    SDLNet_UDP_Send(UDPsocket s, int c, UDPpacket *p)
                                                                   { (void)s;(void)c;(void)p; return 0; }
static inline int    SDLNet_UDP_Recv(UDPsocket s, UDPpacket *p)   { (void)s;(void)p; return 0; }
static inline UDPpacket *SDLNet_AllocPacket(int size)             { (void)size; return NULL; }
static inline void   SDLNet_FreePacket(UDPpacket *p)              { (void)p; }
static inline int    SDLNet_UDP_Bind(UDPsocket s, int c, const IPaddress *a)
                                                                   { (void)s;(void)c;(void)a; return -1; }
static inline IPaddress *SDLNet_UDP_GetPeerAddress(UDPsocket s, int c)
                                                                   { (void)s;(void)c; return NULL; }
static inline const char *SDLNet_GetError(void)                   { return "no network"; }

/* Write/read helpers used by net code */
static inline Uint16 SDLNet_Read16(const void *p) {
    const Uint8 *b = (const Uint8 *)p;
    return (Uint16)((b[0] << 8) | b[1]);
}
static inline Uint32 SDLNet_Read32(const void *p) {
    const Uint8 *b = (const Uint8 *)p;
    return ((Uint32)b[0]<<24)|((Uint32)b[1]<<16)|((Uint32)b[2]<<8)|(Uint32)b[3];
}
static inline void SDLNet_Write16(Uint16 v, void *p) {
    Uint8 *b = (Uint8 *)p;
    b[0] = (v>>8)&0xFF; b[1] = v&0xFF;
}
static inline void SDLNet_Write32(Uint32 v, void *p) {
    Uint8 *b = (Uint8 *)p;
    b[0]=(v>>24)&0xFF; b[1]=(v>>16)&0xFF; b[2]=(v>>8)&0xFF; b[3]=v&0xFF;
}

#endif /* VK_SDL_NET_H */
