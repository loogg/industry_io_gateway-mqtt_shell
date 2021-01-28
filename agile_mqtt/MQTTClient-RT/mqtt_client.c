#include <mqtt_client.h>
#include <string.h>
#include <stdlib.h>

#define DBG_ENABLE
#define DBG_SECTION_NAME    "mqtt"
#define DBG_COLOR
#ifdef PKG_AGILE_MQTT_DEBUG
#define DBG_LEVEL           DBG_LOG
#else
#define DBG_LEVEL           DBG_INFO
#endif
#include <rtdbg.h>

#define mqtt_client_lock(c)             rt_mutex_take(c->lock, RT_WAITING_FOREVER)
#define mqtt_client_unlock(c)           rt_mutex_release(c->lock)

#define mqtt_client_pub_ack_lock(c)     rt_mutex_take(c->pub_ack_lock, RT_WAITING_FOREVER)
#define mqtt_client_pub_ack_unlock(c)   rt_mutex_release(c->pub_ack_lock)

void mqtt_client_set_ops(mqtt_client *c, int (*net_connect)(void *t, const char *server, int port), int (*net_disconnect)(void *t),
                           int (*send_packet)(void *t, const void *buf, int len), int (*net_read)(void *t, uint8_t *buf, int len, int timeout), int (*select)(void *t, int timeout))
{
    c->ops.net_connect = net_connect;
    c->ops.net_disconnect = net_disconnect;
    c->ops.send_packet = send_packet;
    c->ops.net_read = net_read;
    c->ops.select = select;
}

static void mqtt_client_send_pub_ack(mqtt_client *c, mqtt_message_ack *msg_ack)
{
    rt_slist_t *node;
    mqtt_client_pub_ack_lock(c);
    rt_slist_for_each(node, &(c->pub_ack_header))
    {
        struct mqtt_pub_ack_item *item = rt_slist_entry(node, struct mqtt_pub_ack_item, slist);
        rt_mq_send(item->ack_queue, msg_ack, sizeof(mqtt_message_ack));
    }
    mqtt_client_pub_ack_unlock(c);
}

static void mqtt_client_reset_pub_ack(mqtt_client *c)
{
    rt_slist_t *node;
    mqtt_client_pub_ack_lock(c);
    rt_slist_for_each(node, &(c->pub_ack_header))
    {
        struct mqtt_pub_ack_item *item = rt_slist_entry(node, struct mqtt_pub_ack_item, slist);
        rt_mq_control(item->ack_queue, RT_IPC_CMD_RESET, RT_NULL);
    }
    mqtt_client_pub_ack_unlock(c);
}

static struct mqtt_pub_ack_item *mqtt_client_create_pub_ack_item(mqtt_client *c)
{
    rt_mq_t ack_queue = rt_mq_create("mq_ack", sizeof(mqtt_message_ack), 10, RT_IPC_FLAG_FIFO);
    if(ack_queue == RT_NULL)
        return RT_NULL;
    
    struct mqtt_pub_ack_item *item = (struct mqtt_pub_ack_item *)rt_malloc(sizeof(struct mqtt_pub_ack_item));
    if(item == RT_NULL)
    {
        rt_mq_delete(ack_queue);
        return RT_NULL;
    }

    item->ack_queue = ack_queue;
    rt_slist_init(&(item->slist));

    mqtt_client_pub_ack_lock(c);
    rt_slist_append(&(c->pub_ack_header), &(item->slist));
    mqtt_client_pub_ack_unlock(c);

    return item;
}

static void mqtt_client_delete_pub_ack_item(mqtt_client *c, struct mqtt_pub_ack_item *item)
{
    mqtt_client_pub_ack_lock(c);
    rt_slist_remove(&(c->pub_ack_header), &(item->slist));
    item->slist.next = RT_NULL;
    mqtt_client_pub_ack_unlock(c);

    rt_mq_delete(item->ack_queue);
    rt_free(item);
}

static int decode_packet(mqtt_client *c, int *value, int timeout)
{
    unsigned char i;
    int multiplier = 1;
    int len = 0;
    const int MAX_NO_OF_REMAINING_LENGTH_BYTES = 4;

    *value = 0;
    do
    {
        int rc = MQTTPACKET_READ_ERROR;

        if (++len > MAX_NO_OF_REMAINING_LENGTH_BYTES)
        {
            rc = MQTTPACKET_READ_ERROR; /* bad data */
            goto exit;
        }
        rc = c->ops.net_read(c->t, &i, 1, timeout);
        if (rc != 1)
            goto exit;
        *value += (i & 127) * multiplier;
        multiplier *= 128;
    }
    while ((i & 128) != 0);
exit:
    return len;
}

static int read_packet(mqtt_client *c)
{
    int rc = -1;
    MQTTHeader header = {0};
    int len = 0;
    int rem_len = 0;

    /* 1. read the header byte.  This has the packet type in it */
    if (c->ops.net_read(c->t, c->read_buf, 1, 0) != 1)
        goto exit;

    len = 1;
    /* 2. read the remaining length.  This is variable in itself */
    decode_packet(c, &rem_len, 100);
    len += MQTTPacket_encode(c->read_buf + 1, rem_len); /* put the original remaining length back into the buffer */

    /* 3. read the rest of the buffer using a callback to supply the rest of the data */
    if ((rem_len + len) > c->read_bufsz)
        goto exit;
    if (rem_len && (c->ops.net_read(c->t, c->read_buf + len, rem_len, 300) != rem_len))
        goto exit;

    header.byte = c->read_buf[0];
    rc = header.bits.type;
exit:
    return rc;
}

static int get_next_packet_id(mqtt_client *c)
{
    return c->next_packetid = (c->next_packetid == MAX_PACKET_ID) ? 1 : c->next_packetid + 1;
}

static int mqtt_connect(mqtt_client *c)
{
    int rc = -1, len;
    MQTTPacket_connectData *options = &(c->condata);

    c->next_packetid = 0;
    if ((len = MQTTSerialize_connect(c->send_buf, c->send_bufsz, options)) <= 0)
        goto _exit;

    if ((rc = c->ops.send_packet(c->t, c->send_buf, len)) != 0) // send the connect packet
        goto _exit;                     // there was a problem

    {
        int res;
        rt_int32_t timeout = c->pkt_timeout * 1000;

        res = c->ops.select(c->t, timeout);

        if (res <= 0)
        {
            LOG_E("%s wait resp fail, res:%d", __FUNCTION__, res);
            rc = -1;
            goto _exit;
        }
    }

    rc = read_packet(c);
    if (rc < 0)
    {
        LOG_E("%s MQTTPacket_readPacket fail", __FUNCTION__);
        goto _exit;
    }

    if (rc == CONNACK)
    {
        unsigned char sessionPresent, connack_rc;

        if (MQTTDeserialize_connack(&sessionPresent, &connack_rc, c->read_buf, c->read_bufsz) == 1)
        {
            rc = connack_rc;
        }
        else
        {
            rc = -1;
        }
    }
    else
        rc = -1;

_exit:
    return rc;
}

static int mqtt_disconnect(mqtt_client *c)
{
    int rc = -1;
    int len = 0;

    mqtt_client_lock(c);
    len = MQTTSerialize_disconnect(c->send_buf, c->send_bufsz);
    if (len > 0)
        rc = c->ops.send_packet(c->t, c->send_buf, len);            // send the disconnect packet
    mqtt_client_unlock(c);

    return rc;
}

static int mqtt_subscribe(mqtt_client *c)
{
    int rc = -1;
    int len = 0;
    int sub_cnt = 0;
    int qos_sub[MAX_MESSAGE_HANDLERS];
    MQTTString topic_sub[MAX_MESSAGE_HANDLERS];

    for (int i = 0; i < MAX_MESSAGE_HANDLERS; i++)
    {
        topic_sub[i].cstring = NULL;
        topic_sub[i].lenstring.len = 0;
        topic_sub[i].lenstring.data = NULL;
    }

    for (int i = 0; i < MAX_MESSAGE_HANDLERS; i++)
    {
        if(c->message_handlers[i].topicFilter == RT_NULL)
            continue;
        
        qos_sub[sub_cnt] = c->message_handlers[i].qos;
        topic_sub[sub_cnt].cstring = c->message_handlers[i].topicFilter;
        sub_cnt++;
    }

    len = MQTTSerialize_subscribe(c->send_buf, c->send_bufsz, 0, get_next_packet_id(c), sub_cnt, topic_sub, qos_sub);
    if (len <= 0)
        return -1;
    if (c->ops.send_packet(c->t, c->send_buf, len) != 0) // send the subscribe packet
        return -1;                                // there was a problem

    rc = c->ops.select(c->t, c->pkt_timeout * 1000);
    if (rc <= 0)
    {
        LOG_E("%s wait resp fail, rc:%d", __FUNCTION__, rc);
        return -1;
    }

    rc = read_packet(c);
    if (rc < 0)
    {
        LOG_E("MQTTSubscribe MQTTPacket_readPacket MQTTConnect fail");
        return -1;
    }

    if(rc != SUBACK)
        return -1;

    int count = 0, grantedQoS[MAX_MESSAGE_HANDLERS];
    unsigned short mypacketid;

    if (MQTTDeserialize_suback(&mypacketid, sub_cnt, &count, grantedQoS, c->read_buf, c->read_bufsz) != 1)
        return -1;

    if (count != sub_cnt)
    {
        LOG_E("MQTTSubscribe faile, sub cnt:%d, recv cnt:%d", sub_cnt, count);
        return -1;
    }

    for (int i = 0; i < count; i++)
    {
        if (grantedQoS[i] == SUBFAIL)
        {
            LOG_E("Subscribe #%d %s fail!", i, topic_sub[i].cstring);
            return -1;
        }
        
        LOG_I("Subscribe #%d %s OK!", i, topic_sub[i].cstring);
    }

    return 0;
}

static void new_message_data(message_data *md, MQTTString *aTopicName, mqtt_message *aMessage)
{
    md->topic_name = aTopicName;
    md->message = aMessage;
}

// assume topic filter and name is in correct format
// # can only be at end
// + and # can only be next to separator
static char is_topic_matched(char *topicFilter, MQTTString *topicName)
{
    char *curf = topicFilter;
    char *curn = topicName->lenstring.data;
    char *curn_end = curn + topicName->lenstring.len;

    while (*curf && curn < curn_end)
    {
        if (*curn == '/' && *curf != '/')
            break;
        if (*curf != '+' && *curf != '#' && *curf != *curn)
            break;
        if (*curf == '+')
        {
            // skip until we meet the next separator, or end of string
            char *nextpos = curn + 1;
            while (nextpos < curn_end && *nextpos != '/')
                nextpos = ++curn + 1;
        }
        else if (*curf == '#')
            curn = curn_end - 1;    // skip until end of string
        curf++;
        curn++;
    };

    return (curn == curn_end) && (*curf == '\0');
}

static int deliver_message(mqtt_client *c, MQTTString *topicName, mqtt_message *message)
{
    int i;
    int rc = -1;

    // we have to find the right message handler - indexed by topic
    for (i = 0; i < MAX_MESSAGE_HANDLERS; ++i)
    {
        if (c->message_handlers[i].topicFilter != 0 && (MQTTPacket_equals(topicName, c->message_handlers[i].topicFilter) ||
                is_topic_matched(c->message_handlers[i].topicFilter, topicName)))
        {
            if (c->message_handlers[i].callback != NULL)
            {
                message_data md;
                new_message_data(&md, topicName, message);
                c->message_handlers[i].callback(c, &md);
                rc = 0;
            }
        }
    }

    if (rc == -1 && c->default_message_handler != NULL)
    {
        message_data md;
        new_message_data(&md, topicName, message);
        c->default_message_handler(c, &md);
        rc = 0;
    }

    return rc;
}

static int mqtt_cycle(mqtt_client *c)
{
    int len = 0, rc = 0;

    int packet_type = read_packet(c);
    if (packet_type < 0)
    {
        rc = -1;
        goto _exit;
    }

    switch (packet_type)
    {
    case PUBLISH:
    {
        MQTTString topicName;
        mqtt_message msg;
        int intQoS;
        if (MQTTDeserialize_publish(&msg.dup, &intQoS, &msg.retained, &msg.id, &topicName,
                                    (unsigned char **)&msg.payload, (int *)&msg.payloadlen, c->read_buf, c->read_bufsz) != 1)
        {
            rc = -1;
            goto _exit;
        }

        msg.qos = (enum QoS)intQoS;
        deliver_message(c, &topicName, &msg);
        if (msg.qos != QOS0)
        {
            mqtt_client_lock(c);
            if (msg.qos == QOS1)
                len = MQTTSerialize_ack(c->send_buf, c->send_bufsz, PUBACK, 0, msg.id);
            else if (msg.qos == QOS2)
                len = MQTTSerialize_ack(c->send_buf, c->send_bufsz, PUBREC, 0, msg.id);
            if (len <= 0)
                rc = -1;
            else
                rc = c->ops.send_packet(c->t, c->send_buf, len);
            mqtt_client_unlock(c);

            if (rc != 0)
                goto _exit;
        }
    }
    break;

    case PUBACK:
    {
        unsigned char type,dup;
        unsigned short mypacketid;
        if(MQTTDeserialize_ack(&type, &dup, &mypacketid, c->read_buf, c->read_bufsz) != 1)
        {
            rc = -1;
            goto _exit;
        }
        mqtt_message_ack msg_ack;
        msg_ack.packet_id = mypacketid;
        msg_ack.packet_type = PUBACK;
        mqtt_client_send_pub_ack(c, &msg_ack);
    }
    break;

    case PUBREC:
    case PUBREL:
    {
        unsigned short mypacketid;
        unsigned char dup, type;

        mqtt_client_lock(c);
        if (MQTTDeserialize_ack(&type, &dup, &mypacketid, c->read_buf, c->read_bufsz) != 1)
            rc = -1;
        else if ((len = MQTTSerialize_ack(c->send_buf, c->send_bufsz, (type == PUBREC) ? PUBREL : PUBCOMP, 0, mypacketid)) <= 0)
            rc = -1;
        else if ((rc = c->ops.send_packet(c->t, c->send_buf, len)) != 0)
            rc = -1;
        mqtt_client_unlock(c);

        if (rc == -1)
            goto _exit; 
    }
    break;

    case PUBCOMP:
    {
        unsigned char type,dup;
        unsigned short mypacketid;
        if(MQTTDeserialize_ack(&type, &dup, &mypacketid, c->read_buf, c->read_bufsz) != 1)
        {
            rc = -1;
            goto _exit;
        }
        mqtt_message_ack msg_ack;
        msg_ack.packet_id = mypacketid;
        msg_ack.packet_type = PUBCOMP;
        mqtt_client_send_pub_ack(c, &msg_ack);
    }
    break;

    case PINGRESP:
        LOG_D("[%u] recv ping response.", rt_tick_get());
    break;
    
    default:
    break;
    }

_exit:
    return rc;
}

static void mqtt_thread(void *parameter)
{
    mqtt_client *c = (mqtt_client *)parameter;
    int rc, len;

    rt_thread_mdelay(10000);
_mqtt_start:
    if (c->connect_callback)
    {
        c->connect_callback(c);
    }

    rc = c->ops.net_connect(c->t, c->server, c->port);
    if (rc != 0)
    {
        LOG_E("Net connect error(%d).", rc);
        goto _mqtt_restart;
    }

    rc = mqtt_connect(c);
    if (rc != 0)
    {
        LOG_E("MQTT connect error(%d).",  rc);
        goto _mqtt_restart;
    }
    LOG_I("MQTT server connect success.");

    rc = mqtt_subscribe(c);
    if(rc != 0)
    {
        LOG_E("MQTT subscribe error(%d).", rc);
        goto _mqtt_disconnect;
    }
    
    c->connect_cnt++;
    c->connected_time = time(RT_NULL);

    c->isconnected = 1;
    if (c->online_callback)
    {
        c->online_callback(c);
    }

    while(1)
    {
        rc = c->ops.select(c->t, c->keep_alive_interval * 1000);
        if(rc < 0)
        {
            LOG_E("select rc: %d", rc);
            goto _mqtt_disconnect;
        }
        else if(rc == 0)
        {
            LOG_D("[%u] send ping.", rt_tick_get());
            mqtt_client_lock(c);
            len = MQTTSerialize_pingreq(c->send_buf, c->send_bufsz);
            rc = c->ops.send_packet(c->t, c->send_buf, len);
            mqtt_client_unlock(c);
            if(rc != 0)
            {
                LOG_E("[%u] send ping rc: %d ", rt_tick_get(), rc);
                goto _mqtt_disconnect;
            }

            rc = c->ops.select(c->t, c->pkt_timeout * 1000);
            if(rc <= 0)
            {
                LOG_E("[%u] wait Ping Response rc: %d", rt_tick_get(), rc);
                goto _mqtt_disconnect;
            }
        }
        
        if(rc > 0)
        {
            rc = mqtt_cycle(c);
            if(rc < 0)
            {
                LOG_E("MQTT Cycle error(%d).", rc);
                goto _mqtt_disconnect;
            }
        }
    }

_mqtt_disconnect:
    mqtt_disconnect(c);
_mqtt_restart:
    mqtt_client_lock(c);
    c->isconnected = 0;
    mqtt_client_reset_pub_ack(c);
    if (c->offline_callback)
    {
        c->offline_callback(c);
    }
    c->ops.net_disconnect(c->t);
    mqtt_client_unlock(c);

    rt_thread_mdelay(c->reconnect_interval * 1000);
    LOG_D("restart!");
    goto _mqtt_start;
}

int mqtt_start(mqtt_client *c, rt_uint32_t stack_size, rt_uint8_t priority)
{
    static uint8_t counts = 0;
    char name[RT_NAME_MAX];
    rt_memset(name, 0x00, sizeof(name));
    rt_snprintf(name, RT_NAME_MAX, "mqtt%d", counts++);
    c->next_packetid = 0;
    c->isconnected = 0;
    if(c->keep_alive_interval <= 0)
        return -RT_ERROR;
    
    if(c->reconnect_interval <= 0)
        return -RT_ERROR;

    if(c->pkt_timeout <= 0)
        return -RT_ERROR;
    
    c->lock = rt_mutex_create(name, RT_IPC_FLAG_FIFO);
    RT_ASSERT(c->lock != RT_NULL);

    c->pub_ack_lock = rt_mutex_create(name, RT_IPC_FLAG_FIFO);
    RT_ASSERT(c->pub_ack_lock != RT_NULL);
    rt_slist_init(&(c->pub_ack_header));

    c->connected_time = 0;
    c->connect_cnt = 0;
    
    rt_thread_t tid = rt_thread_create(name, mqtt_thread, c, stack_size, priority, 100); 
    RT_ASSERT(tid != RT_NULL);
    rt_thread_startup(tid);

    return RT_EOK;
}

int mqtt_publish(mqtt_client *c, char *topic, enum QoS qos, void *payload, int payload_len)
{
    MQTTString mqtt_topic = MQTTString_initializer;
    mqtt_message_ack msg_ack;
    int len;
    int packetid;
    int rc = 0;
    struct mqtt_pub_ack_item *item = RT_NULL;

    if((c == RT_NULL) || (topic == RT_NULL) || (payload == RT_NULL) || (payload_len == 0))
        return -RT_ERROR;
    
    mqtt_topic.cstring = topic;

    if(qos != QOS0)
    {
        item = mqtt_client_create_pub_ack_item(c);
        if(item == RT_NULL)
            return -RT_ERROR;
    }

    mqtt_client_lock(c);
    if(c->isconnected)
    {
        packetid = get_next_packet_id(c);
        len = MQTTSerialize_publish(c->send_buf, c->send_bufsz, 0, qos, 0, packetid, mqtt_topic, payload, payload_len);
        if(len <= 0)
            rc = -1;
        else
            rc = c->ops.send_packet(c->t, c->send_buf, len);
    }
    else
    {
        rc = -1;
    }
    mqtt_client_unlock(c);

    if(rc != 0)
    {
        if(item)
            mqtt_client_delete_pub_ack_item(c, item);
        return -RT_ERROR;
    }  

    if(qos == QOS0)
        return RT_EOK;

    rt_int32_t timeout = c->pkt_timeout * 1000;
    rt_tick_t enter_tick, leave_tick, diff;
    do
    {
        enter_tick = rt_tick_get();
        rc = rt_mq_recv(item->ack_queue, &msg_ack, sizeof(mqtt_message_ack), timeout);
        if (rc != RT_EOK)
        {
            mqtt_client_delete_pub_ack_item(c, item);
            return -RT_ERROR;
        }
        leave_tick = rt_tick_get();

        if(leave_tick >= enter_tick)
            diff = leave_tick - enter_tick;
        else
            diff = RT_TICK_MAX- enter_tick + leave_tick;

        if (msg_ack.packet_id == packetid)
        {
            mqtt_client_delete_pub_ack_item(c, item);
            return RT_EOK;
        }
        timeout -= diff;
    } while (timeout > 0);

    mqtt_client_delete_pub_ack_item(c, item);
    return -RT_ERROR;
}
