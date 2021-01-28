#include "enet_client.h"
#include <dfs_posix.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>

int enet_net_connect(void *t, const char *server, int port)
{
    int *s = (int *)t;
    int rc = -1;
    *s = -1;
    struct timeval tv;
    struct addrinfo *addr_res = RT_NULL;

    char port_str[10] = "";
    rt_snprintf(port_str, sizeof(port_str), "%d", port);
    
    rc = getaddrinfo(server, port_str, RT_NULL, &addr_res);
    if((rc != 0) || (addr_res == RT_NULL))
    {
        rc = -1;
        goto _exit;
    }

    if((*s = socket(addr_res->ai_family, SOCK_STREAM, 0)) < 0)
    {
        rc = -1;
        goto _exit;
    }

    if((rc = connect(*s, addr_res->ai_addr, addr_res->ai_addrlen)) < 0)
    {
        close(*s);
        *s = -1;

        rc = -2;
        goto _exit;
    }

    /* 20s发送超时 */
    tv.tv_sec = 20;
    tv.tv_usec = 0;
    setsockopt(*s, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv, sizeof(struct timeval));

_exit:
    if(addr_res)
    {
        freeaddrinfo(addr_res);
        addr_res = RT_NULL;
    }
    return rc;
}

int enet_net_disconnect(void *t)
{
    int *s = (int *)t;
    if(*s >= 0)
    {
        close(*s);
        *s = -1;
    }

    return 0;
}

int enet_send_packet(void *t, const void *buf, int len)
{
    int *s = (int *)t;
    int rc;

    if(*s < 0)
        return -1;
    
    if(len <= 0)
        return len;

    rc = send(*s, buf, len, 0);

    if(rc == len)
    {
        rc = 0;
    }
    else
    {
        rc= -1;
    }
    
    return rc;
}

int enet_net_read(void *t, uint8_t *buf, int len, int timeout)
{
    int *s = (int *)t;
    int bytes = 0;
    int rc = 0;

    if(*s < 0)
        return -1;
    
    if(len <= 0)
        return len;

    while(bytes < len)
    {
        rc = recv(*s, &buf[bytes], (size_t)(len - bytes), MSG_DONTWAIT);
        if(rc < 0)
        {
            if(errno != EWOULDBLOCK)
                return -1;
        }
        else
        {
            bytes += rc;
        }

        if(bytes >= len)
            break;
        
        if(timeout > 0)
        {
            fd_set readset, exceptset;
            struct timeval interval;

            interval.tv_sec = timeout / 1000;
            interval.tv_usec = (timeout % 1000) * 1000;

            FD_ZERO(&readset);
            FD_ZERO(&exceptset);
            FD_SET(*s, &readset);
            FD_SET(*s, &exceptset);

            rc = select(*s + 1, &readset, RT_NULL, &exceptset, &interval);
            timeout = 0;
            if(rc < 0)
            {
                return -1;
            }
            else if(rc == 0)
            {
                break;
            }
            else
            {
                if(FD_ISSET(*s, &exceptset))
                {
                    return -1;
                }
            }
        }
        else
        {
            break;
        }
    }

    return bytes;
}

int enet_select(void *t, int timeout)
{
    int *s = (int *)t;
    fd_set readset, exceptset;
    struct timeval interval;

    if(*s < 0)
        return -1;

    interval.tv_sec = timeout / 1000;
    interval.tv_usec = (timeout % 1000) * 1000;

    FD_ZERO(&readset);
    FD_ZERO(&exceptset);
    FD_SET(*s, &readset);
    FD_SET(*s, &exceptset);

    int rc = select(*s + 1, &readset, RT_NULL, &exceptset, &interval);
    if(rc > 0)
    {
        if(FD_ISSET(*s, &exceptset))
        {
            return -1;
        }
    }

    return rc;
}
