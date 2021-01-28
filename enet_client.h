#ifndef __ENET_CLIENT_H
#define __ENET_CLIENT_H
#include <rtthread.h>
#include <rtdevice.h>

int enet_net_connect(void *t, const char *server, int port);
int enet_net_disconnect(void *t);
int enet_send_packet(void *t, const void *buf, int len);
int enet_net_read(void *t, uint8_t *buf, int len, int timeout);
int enet_select(void *t, int timeout);
#endif
