#ifndef __MQTT_SHELL_H
#define __MQTT_SHELL_H
#include <rtthread.h>
#include <rtdevice.h>
#include <mqtt_client.h>

struct mqtt_shell_client
{
    uint8_t isconnected;
    struct rt_ringbuffer *rx_rb;
    struct rt_ringbuffer *tx_rb;
    mqtt_client mqtt;
};

#endif
