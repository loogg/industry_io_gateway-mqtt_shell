#ifndef __MQTT_CLIENT_H
#define __MQTT_CLIENT_H

#include <MQTTPacket.h>
#include <rtthread.h>
#include <rtdevice.h>
#include <time.h>

#ifndef PKG_AGILE_MQTT_SUBSCRIBE_HANDLERS
#define MAX_MESSAGE_HANDLERS    5 /* redefinable - how many subscriptions do you want? */
#else
#define MAX_MESSAGE_HANDLERS    PKG_AGILE_MQTT_SUBSCRIBE_HANDLERS
#endif

#define MAX_PACKET_ID           65535 /* according to the MQTT specification - do not change! */

enum QoS
{
    QOS0,
    QOS1,
    QOS2,
    SUBFAIL = 0x80
};

typedef struct mqtt_message
{
    enum QoS qos;
    uint8_t retained;
    uint8_t dup;
    uint16_t id;
    uint8_t *payload;
    int payloadlen;
} mqtt_message;

typedef struct message_data
{
    mqtt_message *message;
    MQTTString *topic_name;
} message_data;

typedef struct mqtt_message_ack 
{
  uint16_t packet_id;             
  int packet_type;               
} mqtt_message_ack;

struct mqtt_pub_ack_item
{
    rt_mq_t ack_queue;
    rt_slist_t slist;
};

struct mqtt_client_ops
{
    int (*net_connect)(void *t, const char *server, int port);
    int (*net_disconnect)(void *t);
    int (*send_packet)(void *t, const void *buf, int len);
    int (*net_read)(void *t, uint8_t *buf, int len, int timeout);
    int (*select)(void *t, int timeout);
};

typedef struct mqtt_client mqtt_client;

struct mqtt_client
{
    void *t;
    char *server;
    int port;
    MQTTPacket_connectData condata;
    uint16_t next_packetid;
    uint8_t isconnected;
    int send_bufsz;
    int read_bufsz;
    uint8_t *send_buf;
    uint8_t *read_buf;
    int keep_alive_interval;
    int reconnect_interval;
    int pkt_timeout;
    rt_mutex_t lock;
    rt_mutex_t pub_ack_lock;
    rt_slist_t pub_ack_header;
    time_t connected_time;
    uint32_t connect_cnt;

    void (*connect_callback)(mqtt_client *);
    void (*online_callback)(mqtt_client *);
    void (*offline_callback)(mqtt_client *);

    struct message_handler
    {
        char *topicFilter;
        void (*callback)(mqtt_client *, message_data *);
        enum QoS qos;
    } message_handlers[MAX_MESSAGE_HANDLERS]; /* Message handlers are indexed by subscription topic */

    void (*default_message_handler)(mqtt_client *, message_data *);
    struct mqtt_client_ops ops;
};

void mqtt_client_set_ops(mqtt_client *c, int (*net_connect)(void *t, const char *server, int port), int (*net_disconnect)(void *t),
                           int (*send_packet)(void *t, const void *buf, int len), int (*net_read)(void *t, uint8_t *buf, int len, int timeout), int (*select)(void *t, int timeout));
int mqtt_start(mqtt_client *c, rt_uint32_t stack_size, rt_uint8_t priority);
int mqtt_publish(mqtt_client *c, char *topic, enum QoS qos, void *payload, int payload_len);

#endif
