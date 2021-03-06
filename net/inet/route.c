/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		ROUTE i- implementation of the IP router.
 *
 * Version:	@(#)route.c	1.0.14	05/31/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Alan Cox, <gw4pts@gw4pts.ampr.org>
 *		Linus Torvalds, <Linus.Torvalds@helsinki.fi>
 *
 * Fixes:
 *		Alan Cox	:	Verify area fixes.
 *		Alan Cox	:	cli() protects routing changes
 *		Rui Oliveira	:	ICMP routing table updates
 *		(rco@di.uminho.pt)	Routing table insertion and update
 *		Linus Torvalds	:	Rewrote bits to be sensible
 *		Alan Cox	:	Added BSD route gw semantics
 *		Alan Cox	:	Super /proc >4K 
 *		Alan Cox	:	MTU in route table
 *		Alan Cox	: 	MSS actually. Also added the window
 *					clamper.
 *		Sam Lantinga	:	Fixed route matching in rt_del()
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */

#include <asm/segment.h>
#include <asm/system.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/errno.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include "ip.h"
#include "protocol.h"
#include "route.h"
#include "tcp.h"
#include <linux/skbuff.h>
#include "sock.h"
#include "icmp.h"

/*
 *	The routing table list
 */
// 路由表是一个链表
static struct rtable *rt_base = NULL;

/*
 *	Pointer to the loopback route
 */
// 回环路由链表
static struct rtable *rt_loopback = NULL;

/*
 *	Remove a routing table entry.
 */
// 根据目的地址和设备名称删除一个路由项
static void rt_del(unsigned long dst, char *devname)
{
	struct rtable *r, **rp;
	unsigned long flags;

	rp = &rt_base;
	
	/*
	 *	This must be done with interrupts off because we could take
	 *	an ICMP_REDIRECT.
	 */
	 
	save_flags(flags);
	cli();
	// 遍历路由表
	while((r = *rp) != NULL) 
	{
		/* Make sure both the destination and the device match */
		if ( r->rt_dst != dst ||
		(devname != NULL && strcmp((r->rt_dev)->name,devname) != 0) )
		{
			rp = &r->rt_next;
			continue;
		}
		// 利用二级指针，直接修改该指针的值，使他指向被删除节点的下一个节点的地址
		*rp = r->rt_next;
		
		/*
		 *	If we delete the loopback route update its pointer.
		 */
		// 删除的是回环地址 
		if (rt_loopback == r)
			rt_loopback = NULL;
		kfree_s(r, sizeof(struct rtable));
	} 
	restore_flags(flags);
}


/*
 *	Remove all routing table entries for a device. This is called when
 *	a device is downed.
 */
// 删除关联了某个设备的所有路由，用于设备挂掉的时候 
void ip_rt_flush(struct device *dev)
{
	struct rtable *r;
	struct rtable **rp;
	unsigned long flags;

	rp = &rt_base;
	save_flags(flags);
	cli();
	while ((r = *rp) != NULL) {
		if (r->rt_dev != dev) {
			rp = &r->rt_next;
			continue;
		}
		*rp = r->rt_next;
		if (rt_loopback == r)
			rt_loopback = NULL;
		kfree_s(r, sizeof(struct rtable));
	} 
	restore_flags(flags);
}

/*
 *	Used by 'rt_add()' when we can't get the netmask any other way..
 *
 *	If the lower byte or two are zero, we guess the mask based on the
 *	number of zero 8-bit net numbers, otherwise we use the "default"
 *	masks judging by the destination address and our device netmask.
 */
// 获取ip的默认网络掩码 
static inline unsigned long default_mask(unsigned long dst)
{
	dst = ntohl(dst);
	if (IN_CLASSA(dst))
		return htonl(IN_CLASSA_NET);
	if (IN_CLASSB(dst))
		return htonl(IN_CLASSB_NET);
	return htonl(IN_CLASSC_NET);
}


/*
 *	If no mask is specified then generate a default entry.
 */

static unsigned long guess_mask(unsigned long dst, struct device * dev)
{
	unsigned long mask;

	if (!dst)
		return 0;
	// 获取掩码
	mask = default_mask(dst);
	/*
		异或后再和掩码与，如果为0说明网络是不一样的，因为如果网络一样，
		异或后网络部分值为0，再和掩码与也是等于0。所以网络不一样的时候，返回目的ip的掩码，
		否则返回本地主机网络的掩码
	*/
	if ((dst ^ dev->pa_addr) & mask)
		return mask;
	return dev->pa_mask;
}


/*
 *	Find the route entry through which our gateway will be reached
 */
// 查找网关对应的设备，即通过该设备可以达到该网关
static inline struct device * get_gw_dev(unsigned long gw)
{
	struct rtable * rt;

	for (rt = rt_base ; ; rt = rt->rt_next) 
	{	// 查找完都没找到
		if (!rt)
			return NULL;
		/*
			查找和该网关的在同一个网络的ip，从而知道到达该网关的设备，
			如果if里面是true说明网关和当前路由项的目的地址不在一个网络，则不能直接到达，
		*/
		if ((gw ^ rt->rt_dst) & rt->rt_mask)
			continue;
		/* 
		 *	Gateways behind gateways are a no-no 
		 */
		// 到达网关的是另一个网关，说明无法直接到达，一般的网关是直接到达的 
		if (rt->rt_flags & RTF_GATEWAY)
			return NULL;
		return rt->rt_dev;
	}
}

/*
 *	Rewrote rt_add(), as the old one was weird - Linus
 *
 *	This routine is used to update the IP routing table, either
 *	from the kernel (ICMP_REDIRECT) or via an ioctl call issued
 *	by the superuser.
 */
 
void ip_rt_add(short flags, unsigned long dst, unsigned long mask,
	unsigned long gw, struct device *dev, unsigned short mtu, unsigned long window)
{
	struct rtable *r, *rt;
	struct rtable **rp;
	unsigned long cpuflags;

	/*
	 *	A host is a unique machine and has no network bits.
	 */
	 
	if (flags & RTF_HOST) 
	{
		mask = 0xffffffff;
	} 
	
	/*
	 *	Calculate the network mask
	 */
	// 没有传掩码 
	else if (!mask) 
	{	// 和设备对应的ip在同一个网络，则掩码等于设备对应ip的掩码，否则猜测一个掩码
		if (!((dst ^ dev->pa_addr) & dev->pa_mask)) 
		{
			mask = dev->pa_mask;
			// 清除是网关标记位，因为可以直接到到
			flags &= ~RTF_GATEWAY;
			if (flags & RTF_DYNAMIC) 
			{
				/*printk("Dynamic route to my own net rejected\n");*/
				return;
			}
		} 
		else
			mask = guess_mask(dst, dev);
		// 取网络部分
		dst &= mask;
	}
	
	/*
	 *	A gateway must be reachable and not a local address
	 */
	// 清除是网关标记位 
	if (gw == dev->pa_addr)
		flags &= ~RTF_GATEWAY;
	// 如果是一个网关	
	if (flags & RTF_GATEWAY) 
	{
		/*
		 *	Don't try to add a gateway we can't reach.. 
		 */
		// 网关的设备和传进来的设备不一致 
		if (dev != get_gw_dev(gw))
			return;
			
		flags |= RTF_GATEWAY;
	} 
	else
		gw = 0;
		
	/*
	 *	Allocate an entry and fill it in.
	 */
	 
	rt = (struct rtable *) kmalloc(sizeof(struct rtable), GFP_ATOMIC);
	if (rt == NULL) 
	{
		return;
	}
	memset(rt, 0, sizeof(struct rtable));
	rt->rt_flags = flags | RTF_UP;// 可用
	rt->rt_dst = dst;
	rt->rt_dev = dev;
	rt->rt_gateway = gw;
	rt->rt_mask = mask;
	rt->rt_mss = dev->mtu - HEADER_SIZE;
	rt->rt_window = 0;	/* Default is no clamping */

	/* Are the MSS/Window valid ? */

	if(rt->rt_flags & RTF_MSS)
		rt->rt_mss = mtu;
		
	if(rt->rt_flags & RTF_WINDOW)
		rt->rt_window = window;

	/*
	 *	What we have to do is loop though this until we have
	 *	found the first address which has a higher generality than
	 *	the one in rt.  Then we can put rt in right before it.
	 *	The interrupts must be off for this process.
	 */

	save_flags(cpuflags);
	cli();

	/*
	 *	Remove old route if we are getting a duplicate. 
	 */
	// 删除旧的
	rp = &rt_base;
	while ((r = *rp) != NULL) 
	{
		if (r->rt_dst != dst || 
		    r->rt_mask != mask) 
		{
			rp = &r->rt_next;
			continue;
		}
		*rp = r->rt_next;
		if (rt_loopback == r)
			rt_loopback = NULL;
		kfree_s(r, sizeof(struct rtable));
	}
	
	/*
	 *	Add the new route 
	 */
	// 插入链表 
	rp = &rt_base;
	while ((r = *rp) != NULL) {
		
		if ((r->rt_mask & mask) != mask)
			break;
		rp = &r->rt_next;
	}
	rt->rt_next = r;
	*rp = rt;
	
	/*
	 *	Update the loopback route
	 */
	// 如果是回环地址且还没有回环地址则更新rt_loopback链表，所以只有一个回环地址
	if ((rt->rt_dev->flags & IFF_LOOPBACK) && !rt_loopback)
		rt_loopback = rt;
		
	/*
	 *	Restore the interrupts and return
	 */
	 
	restore_flags(cpuflags);
	return;
}


/*
 *	Check if a mask is acceptable.
 */
 
static inline int bad_mask(unsigned long mask, unsigned long addr)
{
	if (addr & (mask = ~mask))
		return 1;
	mask = ntohl(mask);
	if (mask & (mask+1))
		return 1;
	return 0;
}

/*
 *	Process a route add request from the user
 */
 
static int rt_new(struct rtentry *r)
{
	int err;
	char * devname;
	struct device * dev = NULL;
	unsigned long flags, daddr, mask, gw;

	/*
	 *	If a device is specified find it.
	 */
	 
	if ((devname = r->rt_dev) != NULL) 
	{
		err = getname(devname, &devname);
		if (err)
			return err;
		dev = dev_get(devname);
		putname(devname);
		if (!dev)
			return -EINVAL;
	}
	
	/*
	 *	If the device isn't INET, don't allow it
	 */

	if (r->rt_dst.sa_family != AF_INET)
		return -EAFNOSUPPORT;

	/*
	 *	Make local copies of the important bits
	 */
	 
	flags = r->rt_flags;
	daddr = ((struct sockaddr_in *) &r->rt_dst)->sin_addr.s_addr;
	mask = ((struct sockaddr_in *) &r->rt_genmask)->sin_addr.s_addr;
	gw = ((struct sockaddr_in *) &r->rt_gateway)->sin_addr.s_addr;


	/*
	 *	BSD emulation: Permits route add someroute gw one-of-my-addresses
	 *	to indicate which iface. Not as clean as the nice Linux dev technique
	 *	but people keep using it... 
	 */
	 
	if (!dev && (flags & RTF_GATEWAY)) 
	{
		struct device *dev2;
		for (dev2 = dev_base ; dev2 != NULL ; dev2 = dev2->next) 
		{
			if ((dev2->flags & IFF_UP) && dev2->pa_addr == gw) 
			{
				flags &= ~RTF_GATEWAY;
				dev = dev2;
				break;
			}
		}
	}

	/*
	 *	Ignore faulty masks
	 */
	 
	if (bad_mask(mask, daddr))
		mask = 0;

	/*
	 *	Set the mask to nothing for host routes.
	 */
	 
	if (flags & RTF_HOST)
		mask = 0xffffffff;
	else if (mask && r->rt_genmask.sa_family != AF_INET)
		return -EAFNOSUPPORT;

	/*
	 *	You can only gateway IP via IP..
	 */
	 
	if (flags & RTF_GATEWAY) 
	{
		if (r->rt_gateway.sa_family != AF_INET)
			return -EAFNOSUPPORT;
		if (!dev)
			dev = get_gw_dev(gw);
	} 
	else if (!dev)
		dev = ip_dev_check(daddr);

	/*
	 *	Unknown device.
	 */
	 
	if (dev == NULL)
		return -ENETUNREACH;

	/*
	 *	Add the route
	 */
	 
	ip_rt_add(flags, daddr, mask, gw, dev, r->rt_mss, r->rt_window);
	return 0;
}

	
/*
 *	Remove a route, as requested by the user.
 */

static int rt_kill(struct rtentry *r)
{
	struct sockaddr_in *trg;
	char *devname;
	int err;

	trg = (struct sockaddr_in *) &r->rt_dst;
	if ((devname = r->rt_dev) != NULL) 
	{
		err = getname(devname, &devname);
		if (err)
			return err;
	}
	rt_del(trg->sin_addr.s_addr, devname);
	if ( devname != NULL )
		putname(devname);
	return 0;
}


/* 
 *	Called from the PROCfs module. This outputs /proc/net/route.
 */
 
int rt_get_info(char *buffer, char **start, off_t offset, int length)
{
	struct rtable *r;
	int len=0;
	off_t pos=0;
	off_t begin=0;
	int size;

	len += sprintf(buffer,
		 "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\n");
	pos=len;
  
	/*
	 *	This isn't quite right -- r->rt_dst is a struct! 
	 */
	 
	for (r = rt_base; r != NULL; r = r->rt_next) 
	{
        	size = sprintf(buffer+len, "%s\t%08lX\t%08lX\t%02X\t%d\t%lu\t%d\t%08lX\t%d\t%lu\n",
			r->rt_dev->name, r->rt_dst, r->rt_gateway,
			r->rt_flags, r->rt_refcnt, r->rt_use, r->rt_metric,
			r->rt_mask, (int)r->rt_mss, r->rt_window);
		len+=size;
		pos+=size;
		if(pos<offset)
		{
			len=0;
			begin=pos;
		}
		if(pos>offset+length)
			break;
  	}
  	
  	*start=buffer+(offset-begin);
  	len-=(offset-begin);
  	if(len>length)
  		len=length;
  	return len;
}

/*
 *	This is hackish, but results in better code. Use "-S" to see why.
 */
 
#define early_out ({ goto no_route; 1; })

/*
 *	Route a packet. This needs to be fairly quick. Florian & Co. 
 *	suggested a unified ARP and IP routing cache. Done right its
 *	probably a brilliant idea. I'd actually suggest a unified
 *	ARP/IP routing/Socket pointer cache. Volunteers welcome
 */
 
struct rtable * ip_rt_route(unsigned long daddr, struct options *opt, unsigned long *src_addr)
{
	struct rtable *rt;

	for (rt = rt_base; rt != NULL || early_out ; rt = rt->rt_next) 
	{
		if (!((rt->rt_dst ^ daddr) & rt->rt_mask))
			break;
		/*
		 *	broadcast addresses can be special cases.. 
		 */
		if (rt->rt_flags & RTF_GATEWAY)
			continue;		 
		if ((rt->rt_dev->flags & IFF_BROADCAST) &&
		    (rt->rt_dev->pa_brdaddr == daddr))
			break;
	}
	
	if(src_addr!=NULL)
		*src_addr= rt->rt_dev->pa_addr;
		
	if (daddr == rt->rt_dev->pa_addr) {
		if ((rt = rt_loopback) == NULL)
			goto no_route;
	}
	rt->rt_use++;
	return rt;
no_route:
	return NULL;
}

struct rtable * ip_rt_local(unsigned long daddr, struct options *opt, unsigned long *src_addr)
{
	struct rtable *rt;

	for (rt = rt_base; rt != NULL || early_out ; rt = rt->rt_next) 
	{
		/*
		 *	No routed addressing.
		 */
		if (rt->rt_flags&RTF_GATEWAY)
			continue;
		// true则是同一个网络
		if (!((rt->rt_dst ^ daddr) & rt->rt_mask))
			break;
		/*
		 *	broadcast addresses can be special cases.. 
		 */
		// 是广播地址并且设备支持广播
		if ((rt->rt_dev->flags & IFF_BROADCAST) &&
		     rt->rt_dev->pa_brdaddr == daddr)
			break;
	}
	
	if(src_addr!=NULL)
		*src_addr= rt->rt_dev->pa_addr;
	// 目的地址等于设备的地址说明是回环地址
	if (daddr == rt->rt_dev->pa_addr) {
		// 没有可用的回环地址
		if ((rt = rt_loopback) == NULL)
			goto no_route;
	}
	rt->rt_use++;
	return rt;
no_route:
	return NULL;
}

/*
 *	Backwards compatibility
 */
 
static int ip_get_old_rtent(struct old_rtentry * src, struct rtentry * rt)
{
	int err;
	struct old_rtentry tmp;

	err=verify_area(VERIFY_READ, src, sizeof(*src));
	if (err)
		return err;
	memcpy_fromfs(&tmp, src, sizeof(*src));
	memset(rt, 0, sizeof(*rt));
	rt->rt_dst = tmp.rt_dst;
	rt->rt_gateway = tmp.rt_gateway;
	rt->rt_genmask.sa_family = AF_INET;
	((struct sockaddr_in *) &rt->rt_genmask)->sin_addr.s_addr = tmp.rt_genmask;
	rt->rt_flags = tmp.rt_flags;
	rt->rt_dev = tmp.rt_dev;
	printk("Warning: obsolete routing request made.\n");
	return 0;
}

/*
 *	Handle IP routing ioctl calls. These are used to manipulate the routing tables
 */
 
int ip_rt_ioctl(unsigned int cmd, void *arg)
{
	int err;
	struct rtentry rt;

	switch(cmd) 
	{
		case SIOCADDRTOLD:	/* Old style add route */
		case SIOCDELRTOLD:	/* Old style delete route */
			if (!suser())
				return -EPERM;
			err = ip_get_old_rtent((struct old_rtentry *) arg, &rt);
			if (err)
				return err;
			return (cmd == SIOCDELRTOLD) ? rt_kill(&rt) : rt_new(&rt);

		case SIOCADDRT:		/* Add a route */
		case SIOCDELRT:		/* Delete a route */
			if (!suser())
				return -EPERM;
			err=verify_area(VERIFY_READ, arg, sizeof(struct rtentry));
			if (err)
				return err;
			memcpy_fromfs(&rt, arg, sizeof(struct rtentry));
			return (cmd == SIOCDELRT) ? rt_kill(&rt) : rt_new(&rt);
	}

	return -EINVAL;
}
