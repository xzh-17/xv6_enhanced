//
// Created by bear on 10/29/2022.
//
#include "types.h"
#include "errno.h"
#include "defs.h"
#include "slab.h"
#include "debug.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "param.h"
#include "file.h"
#include "mmu.h"
#include "proc.h"

#include "socket.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"

kmem_cache_t *socket_cache;

void socketinit(void)
{
	socket_cache = kmem_cache_create("socket_cache", sizeof(struct socket), 0);
	if (socket_cache == NULL)
	{
		panic("socketinit: socket_cache");
	}
}

struct file *socketalloc(int domain, int type, int protocol, int *err)
{
	if (domain != AF_INET)
	{
		*err = -EINVAL;
		return NULL;
	}

	if (type != SOCK_STREAM && type != SOCK_DGRAM)
	{
		*err = -EINVAL;
		return NULL;
	}

	if (protocol != IPPROTO_TCP && protocol != IPPROTO_UDP)
	{
		*err = -EINVAL;
		return NULL;
	}

	file_t *file = filealloc();
	if (file == NULL)
	{
		*err = -EINVAL;
		return NULL;
	}

	socket_t *socket = kmem_cache_alloc(socket_cache);
	if (socket == NULL)
	{
		*err = -EINVAL;
		fileclose(file);
		return NULL;
	}

	socket->type = type;
	socket->protocol = protocol;

	if (type == SOCK_STREAM)
	{
		socket->pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
	}
	else
	{
		socket->pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
	}

	file->type = FD_SOCKET;
	file->socket = socket;
	file->readable = TRUE;
	file->writable = TRUE;

	*err = 0;
	return file;
}

void socketclose(struct socket *skt)
{
	if (skt->recv_buf)
	{
		pbuf_free(skt->recv_buf);
		skt->recv_buf = NULL;
	}

	if (skt->pcb)
	{
		if (skt->type == SOCK_STREAM)
		{
			tcp_close(skt->pcb);
		}
		else
		{
			udp_remove(skt->pcb);
		}
		tcp_close(skt->pcb); // ? why twice?
		skt->pcb = NULL;
	}

	for (int i = 0; i < SOCKET_NBACKLOG; i++)
	{
		if (skt->backlog[i])
		{
			socketclose(skt->backlog[i]);
			skt->backlog[i] = NULL;
		}
	}

	kmem_cache_free(socket_cache, skt);
}

int socketconnect(struct socket *skt, struct sockaddr *addr, int addr_len)
{
	sockaddr_in_t *addr_in = (sockaddr_in_t *)addr;
	if (skt->protocol != IPPROTO_TCP)
	{
		return -EINVAL;
	}

	struct tcp_pcb *pcb = (struct tcp_pcb *)skt->pcb;
	err_t e = tcp_connect(pcb, &addr_in->sin_addr, addr_in->sin_port, NULL);
	if (e == ERR_OK)
	{
		skt->pid = myproc()->pid;
	}
	return e;
}

int socketbind(struct socket *skt, struct sockaddr *addr, int addr_len)
{
	sockaddr_in_t *addr_in = (sockaddr_in_t *)addr;

	if (skt->protocol != IPPROTO_TCP)
	{
		return -EINVAL;
	}

	struct tcp_pcb *pcb = (struct tcp_pcb *)skt->pcb;
	err_t err = tcp_bind(pcb, &addr_in->sin_addr, addr_in->sin_port);

	if (err == ERR_OK)
	{
		return 0;
	}
	return -EINVAL;
}

int socketlisten(struct socket *skt, int backlog)
{
	if (skt->protocol != IPPROTO_TCP)
	{
		return -EINVAL;
	}

	struct tcp_pcb *pcb = (struct tcp_pcb *)skt->pcb;
	err_t err = 0;
	pcb = tcp_listen_with_backlog_and_err(pcb, backlog, &err);
	skt->pcb = pcb;
	if (err == ERR_OK)
	{
		return 0;
	}

	return -EINVAL;
}

struct file *socketaccept(struct socket *skt, struct sockaddr *addr, int *addrlen, int *err)
{
	if (skt->protocol != IPPROTO_TCP)
	{
		*err = -EINVAL;
		return NULL;
	}

	int avail = 0;
	for (avail = 0; avail < SOCKET_NBACKLOG; avail++)
	{
		if (skt->backlog[avail])
		{
			break;
		}
	}

	if (avail >= SOCKET_NBACKLOG)
	{
		*err = -EAGAIN;
		return NULL;
	}

	file_t *file = filealloc();
	if (file == NULL)
	{
		*err = -EINVAL;
		return NULL;
	}

	socket_t *socket = skt->backlog[avail];

	file->type = FD_SOCKET;
	file->socket = socket;
	file->readable = TRUE;
	file->writable = TRUE;

	struct tcp_pcb *newpcb = socket->pcb;
	*err = 0;

	struct sockaddr_in *in_addr = (struct sockaddr_in *)addr;
	in_addr->sin_addr = newpcb->remote_ip;
	in_addr->sin_port = newpcb->remote_port;
	in_addr->sin_family = AF_INET;

	*addrlen = sizeof(struct sockaddr_in);

	return file;
}

int socketrecv(struct socket *skt, char *buf, int len, int flags)
{
	if (skt->protocol != IPPROTO_TCP)
	{
		return -EINVAL;
	}

	if (len <= 0)
	{
		return -EINVAL;
	}

	if (skt->recv_buf == NULL)
	{
		if (skt->recv_closed)
		{
			return -EINVAL; // FIXME
		}
		return -EAGAIN;
	}

	len = pbuf_copy_partial(skt->recv_buf, buf, len, skt->recv_offset);
	skt->recv_offset += len;
	KDEBUG_ASSERT(skt->recv_offset <= skt->recv_buf->tot_len);

	if (skt->recv_offset == skt->recv_buf->tot_len)
	{
		pbuf_free(skt->recv_buf);
		skt->recv_buf = NULL;
		skt->recv_offset = 0;
	}
	return 0;
}

int socketsend(struct socket *skt, char *buf, int len, int flags)
{
	if (skt->protocol != IPPROTO_TCP)
	{
		return -EINVAL;
	}

	struct tcp_pcb *pcb = (struct tcp_pcb *)skt->pcb;
	if (!pcb)
	{
		return -EINVAL;
	}

	if (!pcb->snd_buf)
	{
		return -EAGAIN;
	}

	len = MIN(len, pcb->snd_buf);

	err_t err = tcp_write(pcb, buf, len, TCP_WRITE_FLAG_COPY);

	if (err == ERR_OK)
	{
		tcp_output(pcb);
		return len;
	}

	if (err == ERR_MEM)
	{
		return -EAGAIN;
	}

	return -EINVAL; // FIXME
}

int socketioctl(struct socket *skt, int req, void *arg)
{
//	struct ifreq *ifreq;
//	struct netdev *dev;
//	struct netif *iface;
//
//	switch (req)
//	{
//	case SIOCGIFINDEX:
//		ifreq = (struct ifreq *)arg;
//		dev = netdev_by_name(ifreq->ifr_name);
//		if (!dev)
//		{
//			return -1;
//		}
//		ifreq->ifr_ifindex = dev->index;
//		break;
//	case SIOCGIFNAME:
//		ifreq = (struct ifreq *)arg;
//		dev = netdev_by_index(ifreq->ifr_ifindex);
//		if (!dev)
//		{
//			return -1;
//		}
//		strncpy(ifreq->ifr_name, dev->name, sizeof(ifreq->ifr_name));
//		break;
//	case SIOCSIFNAME:
//		/* TODO */
//		break;
//	case SIOCGIFHWADDR:
//		ifreq = (struct ifreq *)arg;
//		dev = netdev_by_name(ifreq->ifr_name);
//		if (!dev)
//		{
//			return -1;
//		}
//		/* TODO: HW type check */
//		memcpy(ifreq->ifr_hwaddr.sa_data, dev->addr, dev->alen);
//		break;
//	case SIOCSIFHWADDR:
//		/* TODO */
//		break;
//	case SIOCGIFFLAGS:
//		ifreq = (struct ifreq *)arg;
//		dev = netdev_by_name(ifreq->ifr_name);
//		if (!dev)
//		{
//			return -1;
//		}
//		ifreq->ifr_flags = dev->flags;
//		break;
//	case SIOCSIFFLAGS:
//		ifreq = (struct ifreq *)arg;
//		dev = netdev_by_name(ifreq->ifr_name);
//		if (!dev)
//		{
//			return -1;
//		}
//		if ((dev->flags & IFF_UP) != (ifreq->ifr_flags & IFF_UP))
//		{
//			if (ifreq->ifr_flags & IFF_UP)
//			{
//				dev->ops->open(dev);
//			}
//			else
//			{
//				dev->ops->stop(dev);
//			}
//		}
//		break;
//	case SIOCGIFADDR:
//		ifreq = (struct ifreq *)arg;
//		dev = netdev_by_name(ifreq->ifr_name);
//		if (!dev)
//		{
//			return -1;
//		}
//		iface = netdev_get_netif(dev, ifreq->ifr_addr.sa_family);
//		if (!iface)
//		{
//			return -1;
//		}
//		((struct sockaddr_in *)&ifreq->ifr_addr)->sin_addr = ((struct netif_ip *)iface)->unicast;
//		break;
//	case SIOCSIFADDR:
//		ifreq = (struct ifreq *)arg;
//		dev = netdev_by_name(ifreq->ifr_name);
//		if (!dev)
//		{
//			return -1;
//		}
//		iface = netdev_get_netif(dev, ifreq->ifr_addr.sa_family);
//		if (iface)
//		{
//			if (ip_netif_reconfigure(iface,
//									 ((struct sockaddr_in *)&ifreq->ifr_addr)->sin_addr,
//									 ((struct netif_ip *)iface)->netmask,
//									 ((struct netif_ip *)iface)->gateway) == -1)
//			{
//				return -1;
//			}
//		}
//		else
//		{
//			iface = ip_netif_alloc(((struct sockaddr_in *)&ifreq->ifr_addr)->sin_addr, 0xffffffff, 0);
//			if (!iface)
//			{
//				return -1;
//			}
//			netdev_add_netif(dev, iface);
//		}
//		break;
//	case SIOCGIFNETMASK:
//		ifreq = (struct ifreq *)arg;
//		dev = netdev_by_name(ifreq->ifr_name);
//		if (!dev)
//		{
//			return -1;
//		}
//		iface = netdev_get_netif(dev, ifreq->ifr_addr.sa_family);
//		if (!iface)
//		{
//			return -1;
//		}
//		((struct sockaddr_in *)&ifreq->ifr_netmask)->sin_addr = ((struct netif_ip *)iface)->netmask;
//		break;
//	case SIOCSIFNETMASK:
//		ifreq = (struct ifreq *)arg;
//		dev = netdev_by_name(ifreq->ifr_name);
//		if (!dev)
//		{
//			return -1;
//		}
//		iface = netdev_get_netif(dev, ifreq->ifr_addr.sa_family);
//		if (!iface)
//		{
//			return -1;
//		}
//		if (ip_netif_reconfigure(iface,
//								 ((struct netif_ip *)iface)->unicast,
//								 ((struct sockaddr_in *)&ifreq->ifr_addr)->sin_addr,
//								 ((struct netif_ip *)iface)->gateway) == -1)
//		{
//			return -1;
//		}
//		break;
//	case SIOCGIFBRDADDR:
//		ifreq = (struct ifreq *)arg;
//		dev = netdev_by_name(ifreq->ifr_name);
//		if (!dev)
//		{
//			return -1;
//		}
//		iface = netdev_get_netif(dev, ifreq->ifr_addr.sa_family);
//		if (!iface)
//		{
//			return -1;
//		}
//		((struct sockaddr_in *)&ifreq->ifr_broadaddr)->sin_addr = ((struct netif_ip *)iface)->broadcast;
//		break;
//	case SIOCSIFBRDADDR:
//		/* TODO */
//		break;
//	case SIOCGIFMTU:
//		ifreq = (struct ifreq *)arg;
//		dev = netdev_by_name(ifreq->ifr_name);
//		if (!dev)
//		{
//			return -1;
//		}
//		ifreq->ifr_mtu = dev->mtu;
//		break;
//	case SIOCSIFMTU:
//		break;
//	default:
//		return -1;
//	}
//	return 0;
	return 0;
}

err_t lwip_tcp_event(void *arg, struct tcp_pcb *pcb, enum lwip_event event, struct pbuf *p,
					 u16_t size, err_t err)
{
	struct socket *socket = arg;
	pid_t pid = socket->pid;
	int r;

	switch (event)
	{
	case LWIP_EVENT_ACCEPT:
		if (err == ERR_OK)
		{
			int free = 0;
			for (free = 0; free < SOCKET_NBACKLOG; ++free)
			{
				if (!socket->backlog[free])
				{
					break;
				}
			}
			/* queue full */
			if (free >= SOCKET_NBACKLOG)
			{
				return ERR_MEM;
			}
			KDEBUG_MSG_ASSERT(pcb->listener == socket->pcb, "listener");
			socket_t *newsocket = kmem_cache_alloc(socket_cache);
			memset(newsocket, 0, sizeof(struct socket));

			/* the passed in pcb is for the new socket */
			newsocket->pcb = pcb;
			tcp_arg(pcb, newsocket);
			newsocket->protocol = socket->protocol;
			newsocket->type = socket->type;

			socket->backlog[free] = newsocket;
		}
		return ERR_OK;
	case LWIP_EVENT_SENT:
		/* ignore */
		return ERR_OK;
	case LWIP_EVENT_RECV:
		/* closed or error */
		if (!p || err != ERR_OK)
		{
			if (p)
			{
				pbuf_free(p);
			}
			socket->recv_closed = TRUE;
			return ERR_OK;
		}
		/* buffer hasn't been received */
		if (socket->recv_buf)
		{
			return ERR_MEM;
		}
		/* ack the packet */
		tcp_recved(socket->pcb, p->tot_len);
		socket->recv_buf = p;
		socket->recv_offset = 0;
		return ERR_OK;
	case LWIP_EVENT_CONNECTED:
		KDEBUG_MSG_ASSERT(pid != 0, "connect must have been called");
		/* reset */
		socket->pid = 0;
		/* reply */
		r = (err == ERR_OK) ? 0 : -EINVAL; // FIXME: -ECONNRESET;
		r = sys_send(pid, r, virt_to_pn(pong), 0, -1);
		assert(r == 0, "sys_send");
		return ERR_OK;
	case LWIP_EVENT_POLL:
		/* ignore */
		return ERR_OK;
	case LWIP_EVENT_ERR:
		/* pcb is already deallocated */
		socket->pcb = NULL;
		/* let gc do the job: free_socket(socket); */
		return ERR_ABRT;
	default:
		break;
	}

	KDEBUG_UNREACHABLE;
}