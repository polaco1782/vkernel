/*
 * net_drivers_vk.c - Network drivers for vkernel (loopback only)
 * Replaces src/net/src/net_drivers.c
 */

#include "net_dgrm.h"
#include "net_loop.h"
#include "net_udp.h"

/* Only the loopback driver — no datagram/UDP */
net_driver_t net_drivers[MAX_NET_DRIVERS] = {
    {
        "Loopback",
        false,
        Loop_Init,
        Loop_Listen,
        Loop_SearchForHosts,
        Loop_Connect,
        Loop_CheckNewConnections,
        Loop_GetMessage,
        Loop_SendMessage,
        Loop_SendUnreliableMessage,
        Loop_CanSendMessage,
        Loop_CanSendUnreliableMessage,
        Loop_Close,
        Loop_Shutdown
    },
};
i32 net_numdrivers = 1;
