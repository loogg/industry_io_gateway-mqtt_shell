#include "mqtt_shell.h"
#include "init_module.h"
#include "plugins.h"
#include <agile_console.h>
#include "enet_client.h"

#define DBG_TAG "mqtt_shell"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define MQTT_SERVER     "119.23.105.150"
#define MQTT_PORT       8002
#define MQTT_USERNAME   ""
#define MQTT_PASSWORD   ""

static struct plugins_module mqtt_shell_plugin = {
	.name = "mqtt_shell",
	.version = "v1.0.0",
	.author = "malongwei"
};

static struct init_module mqtt_shell_init_module;
static struct mqtt_shell_client client = {0};
static struct agile_console_backend mqtt_backend = {0};

static void handler_shell_receive(mqtt_client *c_m, message_data *md)
{
    rt_base_t level = rt_hw_interrupt_disable();
    rt_ringbuffer_put(client.rx_rb, md->message->payload, md->message->payloadlen);
    rt_hw_interrupt_enable(level);
}

static void wait_connect(void)
{
    while(client.mqtt.isconnected == 0)
    {
        rt_thread_mdelay(200);
    }
}

static void mqtt_shell_entry(void* parameter)
{
    mqtt_shell_plugin.state = PLUGINS_STATE_RUNNING;

    const struct global_sundry *g_sundry = global_sundry_get();
    char pub_topic[100] = "";
    snprintf(pub_topic, sizeof(pub_topic), "%s/shell/send", g_sundry->serial_number);
    char *login_msg = "hello,baba\r\n";

    uint8_t tx_buffer[200];
    rt_base_t level;

_mqtt_shell_start:
    wait_connect();
    mqtt_publish(&(client.mqtt), pub_topic, QOS0, login_msg, rt_strlen(login_msg));

    LOG_I("Connect success.");

    level = rt_hw_interrupt_disable();
    rt_ringbuffer_reset(client.tx_rb);
    rt_hw_interrupt_enable(level);

    client.isconnected = 1;

    while(1)
    {
        rt_thread_mdelay(5);
        if(client.mqtt.isconnected == 0)
            break;
        
        level = rt_hw_interrupt_disable();
        int send_len = rt_ringbuffer_get(client.tx_rb, tx_buffer, sizeof(tx_buffer));
        rt_hw_interrupt_enable(level);

        if(send_len <= 0)
        {
            rt_thread_mdelay(50);
            continue;
        }

        mqtt_publish(&(client.mqtt), pub_topic, QOS0, tx_buffer, send_len);
    }

    client.isconnected = 0;
    LOG_D("restart!");
    goto _mqtt_shell_start;
}

static void mqtt_backend_output(const uint8_t *buf, int len)
{
    if(client.isconnected == 0)
        return;

    rt_base_t level = rt_hw_interrupt_disable();
    rt_ringbuffer_put(client.tx_rb, buf, len);
    rt_hw_interrupt_enable(level);
}

static int mqtt_backend_read(uint8_t *buf, int len)
{
    if(client.isconnected == 0)
        return 0;

    rt_size_t result = 0;

    rt_base_t level = rt_hw_interrupt_disable();
    result = rt_ringbuffer_get(client.rx_rb, buf, len);
    rt_hw_interrupt_enable(level);

    return result;
}

static int mqtt_shell_init(void)
{
    const struct global_sundry *g_sundry = global_sundry_get();

    rt_memset(&client, 0, sizeof(struct mqtt_shell_client));

    client.rx_rb = rt_ringbuffer_create(1024);
    RT_ASSERT(client.rx_rb != RT_NULL);

    client.tx_rb = rt_ringbuffer_create(10240);
    RT_ASSERT(client.tx_rb != RT_NULL);

    client.mqtt.t = rt_malloc(sizeof(int));
    *((int *)(client.mqtt.t)) = -1;
    client.mqtt.server = MQTT_SERVER;
    client.mqtt.port = MQTT_PORT;
    MQTTPacket_connectData condata = MQTTPacket_connectData_initializer;
    rt_memcpy(&(client.mqtt.condata), &condata, sizeof(MQTTPacket_connectData));
    client.mqtt.condata.clientID.cstring = rt_strdup(g_sundry->serial_number);
    client.mqtt.condata.keepAliveInterval = 300;
    client.mqtt.condata.cleansession = 1;
    client.mqtt.condata.username.cstring = MQTT_USERNAME;
    client.mqtt.condata.password.cstring = MQTT_PASSWORD;
    client.mqtt.send_bufsz = 2048;
    client.mqtt.read_bufsz = 2048;
    client.mqtt.send_buf = (uint8_t *)rt_malloc(client.mqtt.send_bufsz);
    client.mqtt.read_buf = (uint8_t *)rt_malloc(client.mqtt.read_bufsz);
    client.mqtt.keep_alive_interval = 120;
    client.mqtt.reconnect_interval = 2;
    client.mqtt.pkt_timeout = 10;
    char sub_topic[100] = "";
    snprintf(sub_topic, sizeof(sub_topic), "%s/shell/receive", g_sundry->serial_number);
    client.mqtt.message_handlers[0].topicFilter = rt_strdup(sub_topic); //服务器端向网关子设备发起RPC调用
    client.mqtt.message_handlers[0].qos = QOS0;
    client.mqtt.message_handlers[0].callback = handler_shell_receive;
    mqtt_client_set_ops(&(client.mqtt), enet_net_connect, enet_net_disconnect, enet_send_packet, enet_net_read, enet_select);
    mqtt_start(&(client.mqtt), 2048, 24);

    rt_thread_t tid = rt_thread_create("mqtt_sh", mqtt_shell_entry, RT_NULL, 4096, 24, 100);
    RT_ASSERT(tid != RT_NULL);
    rt_thread_startup(tid);

    mqtt_backend.output = mqtt_backend_output;
    mqtt_backend.read = mqtt_backend_read;

    agile_console_backend_register(&mqtt_backend);

    return RT_EOK;
}

int fregister(const char *path, void *dlmodule, uint8_t is_sys)
{
    plugins_register(&mqtt_shell_plugin, path, dlmodule, is_sys);

    mqtt_shell_init_module.init = mqtt_shell_init;
    init_module_app_register(&mqtt_shell_init_module);

    return RT_EOK;
}
