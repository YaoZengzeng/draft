#include <linux/module.h>
#include <linux/init.h>
#include <linux/moduleparam.h>

#include <linux/sched.h>
#include <linux/kernel.h> /* printk() */
#include <linux/slab.h> /* kmalloc() */
#include <linux/errno.h>  /* error codes */
#include <linux/types.h>  /* size_t */
#include <linux/interrupt.h> /* mark_bh */

#include <linux/in.h>
#include <linux/netdevice.h>   /* struct device, and other headers */
#include <linux/etherdevice.h> /* eth_type_trans */
#include <linux/ip.h>          /* struct iphdr */
#include <linux/tcp.h>         /* struct tcphdr */
#include <linux/skbuff.h>

#include <linux/in6.h>
#include <asm/checksum.h>

// These are the flags in the statusword
#define SNULL_RX_INTR	0x0001
#define SNULL_TX_INTR	0x0002

// Default timeout period
#define SNULL_TIMEOUT	5	// In jiffies

// Transmitter lockup simulation, normally disabled.
static int lockup = 0;

static int timeout = SNULL_TIMEOUT;

// The devices
struct net_device *snull_devs[2];

// A structure representing an in-flight packet.
struct snull_packet {
	struct snull_packet *next;
	struct net_device *dev;
	int datalen;
	u8 data[ETH_DATA_LEN];
};

int pool_size = 8;

// This structure is private to each device. It is used to pass
// packets in and out, so there is place for a packet
struct snull_priv {
	struct net_device_stats	stats;
	int status;
	struct snull_packet *ppool;
	struct snull_packet *rx_queue;	// List of incoming packets
	int rx_int_enabled;
	int tx_packetlen;
	u8 *tx_packetdata;
	struct sk_buff *skb;
	spinlock_t lock;
};

static void snull_tx_timeout(struct net_device *dev);
static void (*snull_interrupt)(int, void *, struct pt_regs *);
void snull_rx(struct net_device *dev, struct snull_packet *pkt);

// Set up a device's packet pool
void snull_setup_pool(struct net_device *dev) {
	struct snull_priv *priv = netdev_priv(dev);
	int i;
	struct snull_packet *pkt;

	priv->ppool = NULL;
	for (i = 0; i < pool_size; i++) {
		pkt = kmalloc(sizeof (struct snull_packet), GFP_KERNEL);
		if (pkt == NULL) {
			printk(KERN_NOTICE "Ran out of memory allocating packet pool\n");
			return;
		}
		pkt->dev = dev;
		pkt->next = priv->ppool;
		priv->ppool = pkt;
	}
}

void snull_teardown_pool(struct net_device *dev) {
	struct snull_priv *priv = netdev_priv(dev);
	struct snull_packet *pkt;

	while((pkt = priv->ppool)) {
		priv->ppool = pkt->next;
		kfree(pkt);
	}
}

// Buffer/pool management
struct snull_packet *snull_get_tx_buffer(struct net_device *dev) {
	struct snull_priv *priv = netdev_priv(dev);
	unsigned long flags;
	struct snull_packet *pkt;

	spin_lock_irqsave(&priv->lock, flags);
	pkt = priv->ppool;
	priv->ppool = pkt->next;
	if (priv->ppool == NULL) {
			printk("Pool empty\n");
			netif_stop_queue(dev);
	}
	spin_unlock_irqrestore(&priv->lock, flags);
	return pkt;
}

void snull_release_buffer(struct snull_packet *pkt) {
	unsigned long flags;
	struct snull_priv *priv = netdev_priv(pkt->dev);

	spin_lock_irqsave(&priv->lock, flags);
	pkt->next = priv->ppool;
	priv->ppool = pkt;
	spin_unlock_irqrestore(&priv->lock, flags);
	if (netif_queue_stopped(pkt->dev) && pkt->next == NULL) {
		netif_wake_queue(pkt->dev);
	}
}

void snull_enqueue_buf(struct net_device *dev, struct snull_packet *pkt) {
	unsigned long flags;
	struct snull_priv *priv = netdev_priv(dev);

	spin_lock_irqsave(&priv->lock, flags);
	pkt->next = priv->rx_queue;
	priv->rx_queue = pkt;
	spin_unlock_irqrestore(&priv->lock, flags);
}

struct snull_packet *snull_dequeue_buf(struct net_device *dev) {
	struct snull_priv *priv = netdev_priv(dev);
	struct snull_packet *pkt;
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	pkt = priv->rx_queue;
	if (pkt != NULL) {
		priv->rx_queue = pkt->next;
	}
	spin_unlock_irqrestore(&priv->lock, flags);
	return pkt;
}

// Enable and disable receive interrupts
static void snull_rx_ints(struct net_device *dev, int enable) {
	struct snull_priv *priv = netdev_priv(dev);
	priv->rx_int_enabled = enable;
}

// Open and close
int snull_open(struct net_device *dev) {
	// request_region(), request_irq(), ... (like fops->open)

	// Assign the hardware address of the board: use "\0SNULx", where
	// x is 0 or 1. The first byte is '\0' to avoid being a multicast
	// address (the first byte of multicast addrs is odd).
	memcpy(dev->dev_addr, "\0SNUL0", ETH_ALEN);
	if (dev == snull_devs[1]) {
		dev->dev_addr[ETH_ALEN - 1]++;	//\0SNUL1
	}
	netif_start_queue(dev);
	return 0;
}

int snull_release(struct net_device *dev) {
	// release ports, irq and such -- like fops->close

	netif_stop_queue(dev);	// can't transmit any more
	return 0;
}

// Configuraion changes (passed on by ifconfig)
int snull_config(struct net_device *dev, struct ifmap *map) {
	return 0;
}

// The typical interrupt entry point
static void snull_regular_interrupt(int irq, void *dev_id, struct pt_regs *regs) {
	int statusword;
	struct snull_priv *priv;
	struct snull_packet *pkt = NULL;

	// As usual, check the "device" pointer to be sure it is
	// really interrupting.
	// Then assign "struct device *dev" 
	struct net_device *dev = (struct net_device *)dev_id;
	// ... and check with hw if it's really ours

	// paranoid
	if (!dev) {
		return;
	}

	// Lock the device
	priv = netdev_priv(dev);
	spin_lock(&priv->lock);

	// retrieve statusword: real netdevices use I/O instructions
	statusword = priv->status;
	priv->status = 0;
	if (statusword & SNULL_RX_INTR) {
		// send it to snull_rx for handling
		pkt = priv->rx_queue;
		if (pkt) {
			priv->rx_queue = pkt->next;
			snull_rx(dev, pkt);
		}
	}
	if (statusword & SNULL_TX_INTR) {
		// a transmission is over: free the skb
		priv->stats.tx_packets++;
		priv->stats.tx_bytes += priv->tx_packetlen;
		dev_kfree_skb(priv->skb);
	}

	// Unlock the device and we are done
	spin_unlock(&priv->lock);
	if (pkt) {
		snull_release_buffer(pkt);	// Do this outside the lock
	}

	return;
}

// Ioctl commands
int snull_ioctl(struct net_device *dev, struct ifreq *rq, int cmd) {
	return 0;
}

// Deal with a transmit timeout
void snull_tx_timeout(struct net_device *dev) {
	struct snull_priv *priv = netdev_priv(dev);

	printk("Transmit timeout at %ld, latency %ld\n", jiffies,
			jiffies - dev->trans_start);
	// Simulate a transmission interrupt to get things move
	priv->status = SNULL_TX_INTR;
	snull_interrupt(0, dev, NULL);
	priv->stats.tx_errors++;
	netif_wake_queue(dev);

	return;
}

// Return statistics to the caller
struct net_device_stats *snull_stats(struct net_device *dev) {
	struct snull_priv *priv = netdev_priv(dev);
	return &priv->stats;
}

// This function is called to fill up an eth header, since arp
// is not available on the interface
int snull_header(struct sk_buff *skb, struct net_device *dev,
				unsigned short type, void *daddr, void *saddr,
				unsigned int len) {
	struct ethhdr *eth = (struct ethhdr *)skb_push(skb, ETH_HLEN);

	eth->h_proto = htons(type);
	memcpy(eth->h_source, saddr ? saddr : dev->dev_addr, dev->addr_len);
	memcpy(eth->h_dest, daddr ? daddr : dev->dev_addr, dev->addr_len);
	eth->h_dest[ETH_ALEN-1] ^= 0x01;	// dest is us xor 1
	return (dev->hard_header_len);
}

// Receive a packet: retrieve, encapsulate and pass over to upper levels
void snull_rx(struct net_device *dev, struct snull_packet *pkt) {
	struct sk_buff *skb;
	struct snull_priv *priv = netdev_priv(dev);

	// The packet has been retrieved from the transmission
	// medium. Build an skb around it, so upper layers can handle it
	skb = dev_alloc_skb(pkt->datalen + 2);
	if (!skb) {
		if (printk_ratelimit()) {
			printk("snull rx: low on mem - packet dropped\n");
			priv->stats.rx_dropped++;
			goto out;
		}
	}
	skb_reserve(skb, 2);	// align IP on 16B boundary
	memcpy(skb_put(skb, pkt->datalen), pkt->data, pkt->datalen);

	// Write metadata, and then pass to the receive level
	skb->dev = dev;
	skb->protocol = eth_type_trans(skb, dev);
	skb->ip_summed = CHECKSUM_UNNECESSARY;	// dont check it
	priv->stats.rx_packets++;
	priv->stats.rx_bytes += pkt->datalen;
	netif_rx(skb);
out:
	return;
}

// Transmit a packet (low level interface)
static void snull_hw_tx(char *buf, int len, struct net_device *dev) {
	// This function deals with hw details. This function loops
	// back the packet to the other snull interface (if any).
	// In other words, this function implements the snull behaviour,
	// while all other procedures are rather device-independent
	struct iphdr *ih;
	struct net_device *dest;
	struct snull_priv *priv;
	u32 *saddr, *daddr;
	struct snull_packet *tx_buffer;

	if (len < sizeof(struct ethhdr) + sizeof(struct iphdr)) {
		printk("snull: Hmm... packet too short (%i octets)\n", len);
		return;
	}

	// Ethhdr is 14 bytes, but the kernel arranges for iphdr
	// to be aligned (i.e., ethhdr is unaligned)
	ih = (struct iphdr *)(buf+sizeof(struct ethhdr));
	saddr = &ih->saddr;
	daddr = &ih->daddr;

	((u8 *)saddr)[2] ^= 1;	// change the third octet (class C)
	((u8 *)daddr)[2] ^= 1;

	ih->check = 0;		// and rebuild the checksum (ip needs it)
	ih->check = ip_fast_csum((unsigned char *)ih, ih->ihl);

	if (dev == snull_devs[0]) {
		printk("%08x:%05i --> %08x:%05i\n",
				ntohl(ih->saddr), ntohs(((struct tcphdr*)(ih+1))->source),
				ntohl(ih->daddr), ntohs(((struct tcphdr*)(ih+1))->dest));
	} else {
		printk("%08x:%05i <-- %08x:%05i\n",
				ntohl(ih->daddr), ntohs(((struct tcphdr*)(ih+1))->dest),
				ntohl(ih->saddr), ntohs(((struct tcphdr*)(ih+1))->source));
	}

	// Ok, now the packet is ready for transmissin: first simulate a
	// receive interrupt on the twin device, then a transmission-done
	// on the transmitting device
	dest = snull_devs[dev == snull_devs[0] ? 1 : 0];
	priv = netdev_priv(dest);
	tx_buffer = snull_get_tx_buffer(dev);
	tx_buffer->datalen = len;
	memcpy(tx_buffer->data, buf, len);
	snull_enqueue_buf(dest, tx_buffer);
	if (priv->rx_int_enabled) {
		priv->status |= SNULL_RX_INTR;
		snull_interrupt(0, dest, NULL);
	}

	priv = netdev_priv(dev);
	priv->tx_packetlen = len;
	priv->tx_packetdata = buf;
	priv->status |= SNULL_TX_INTR;
	snull_interrupt(0, dev, NULL);
}

// Transmit a packet (called by the kernel)
int snull_tx(struct sk_buff *skb, struct net_device *dev) {
	int len;
	char *data, shortpkt[ETH_ZLEN];
	struct snull_priv *priv = netdev_priv(dev);

	data = skb->data;
	len = skb->len;
	if (len < ETH_ZLEN) {
		memset(shortpkt, 0, ETH_ZLEN);
		memcpy(shortpkt, skb->data, skb->len);
		len = ETH_ZLEN;
		data = shortpkt;
	}
	dev->trans_start = jiffies;	// Save the timestamp

	// Remember the skb, so we can free it at interrupt time
	priv->skb = skb;

	// actual deliver of data is device-specific, and not shown here
	snull_hw_tx(data, len, dev);

	return 0;	// Our simple device can not fail
}

static const struct net_device_ops snull_netdev_ops = {
	.ndo_open		= snull_open,
	.ndo_stop		= snull_release,
	.ndo_set_config	= snull_config,
	.ndo_start_xmit	= snull_tx,
	.ndo_do_ioctl	= snull_ioctl,
	.ndo_get_stats	= snull_stats,
	//.ndo_change_mtu	= snull_change_mtu,
	.ndo_tx_timeout	= snull_tx_timeout,
};

static const struct header_ops snull_header_ops = {
	.create	= snull_header,
	.cache	= NULL,
};

// The init function (sometimes called probe)
// It is invoked by register_netdev()
void snull_init(struct net_device *dev) {
	struct snull_priv *priv;
#if 0
	// Make the usual checks: check_region(), probe irq, ...
	// -NODEV should returned if no device found. No resource
	// should be grabbed: this is done on open()
#endif
	// Then assign other fields in dev, using ether_setup() and
	// some hand assignments
	ether_setup(dev);	// assign some of the fields

	dev->watchdog_timeo = timeout;

	// keep the default flags, just add NOARP
	dev->flags	|= IFF_NOARP;
	dev->features |= NETIF_F_HW_CSUM;
	dev->netdev_ops = &snull_netdev_ops;
	dev->header_ops = &snull_header_ops;

	// Then, initialize the priv field. This encloses the statistics
	// and a few private fields
	priv = netdev_priv(dev);
	memset(priv, 0, sizeof(struct snull_priv));
	spin_lock_init(&priv->lock);
	snull_rx_ints(dev, 1);	// enable receive interrupts
	snull_setup_pool(dev);
}


// Finally, the module stuff

void snull_cleanup(void) {
	int i;

	for (i = 0; i < 2; i++) {
		if (snull_devs[i]) {
			unregister_netdev(snull_devs[i]);
			snull_teardown_pool(snull_devs[i]);
			free_netdev(snull_devs[i]);
		}
	}
}

int snull_init_module(void) {
	int result, i, ret = -ENOMEM;

	snull_interrupt = snull_regular_interrupt;

	// Allocate the devices
	snull_devs[0] = alloc_netdev(sizeof(struct snull_priv), "sn%d",
				snull_init);
	snull_devs[1] = alloc_netdev(sizeof(struct snull_priv), "sn%d",
				snull_init);
	if (snull_devs[0] == NULL || snull_devs[1] == NULL) {
		goto out;
	}

	ret = -ENODEV;
	for (i = 0; i < 2; i++) {
		if ((result = register_netdev(snull_devs[i]))) {
			printk("snull: error %i registering device %s\n",
					result, snull_devs[i]->name);
		} else {
			ret = 0;
		}
	}

out:
	if (ret) {
		snull_cleanup();
	}
	return ret;
}

module_init(snull_init_module);
module_exit(snull_cleanup);
