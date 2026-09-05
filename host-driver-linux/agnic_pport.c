// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * mamoru-agnic P4/P5: the front-panel port netdevs (port1..port9) + the pport demux.
 *
 * The single GIU trunk carries ALL front-panel ports multiplexed; every frame (both
 * directions) is prefixed with 66 bytes: [byte0 = 0x81+port][byte1 = 0x00][64-byte HW
 * header][ethernet]. RX reads byte0 (port = byte0 - 0x81), strips the 66-byte prefix,
 * and delivers on that port's netdev; TX prepends 66 bytes (stamping byte0) and hands
 * the frame to the single trunk TX ring (agnic_giu_tx). The GIU trunk itself is never
 * exposed as an OS-visible netdev.
 *
 * Clean-room Linux port of the proven BSD-2 FreeBSD agnic_pport.c. No NPU reset/FLR.
 */
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/sysfs.h>
#include <linux/skbuff.h>
#include "agnic.h"

#define AGNIC_DRV_VERSION "1.0"

/* TX 64-byte HW-header fill (the current NPU treats it as opaque):
 *   0 = zeros, 1 = replay the snapshotted RX header (zeros until one is seen),
 *   3 = SFOS-exact header[k] = 0xC0 + k. Default 3 (the FreeBSD default). */
static int tx_hdr_mode = 3;
module_param(tx_hdr_mode, int, 0644);
MODULE_PARM_DESC(tx_hdr_mode, "pport TX 64-byte header fill: 0=zeros 1=replay-RX 3=0xC0+k (default 3)");

static int agnic_pport_open(struct net_device *ndev)
{
	netif_carrier_on(ndev);
	netif_start_queue(ndev);
	return 0;
}

static int agnic_pport_stop(struct net_device *ndev)
{
	netif_stop_queue(ndev);
	netif_carrier_off(ndev);
	return 0;
}

/* ndo_start_xmit: prepend the 66-byte prefix, stamp the dst port, hand to the trunk. */
static netdev_tx_t agnic_pport_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct agnic_port *p = netdev_priv(ndev);
	struct agnic *ag = p->ag;
	u32 frame_len = skb->len;
	u8 *d;

	if (skb_cow_head(skb, AGNIC_PPORT_PREFIX_LEN)) {
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	d = skb_push(skb, AGNIC_PPORT_PREFIX_LEN);
	d[0] = AGNIC_PPORT_TAG0(p->index);	/* 0x81 + index */
	d[1] = 0x00;

	/* Fill the 64-byte HW header at d + 2. */
	switch (tx_hdr_mode) {
	case 1:
		if (ag->rx_last_hdr_valid)
			memcpy(d + AGNIC_PPORT_TAG_LEN, ag->rx_last_hdr, AGNIC_PPORT_HDR_LEN);
		else
			memset(d + AGNIC_PPORT_TAG_LEN, 0, AGNIC_PPORT_HDR_LEN);
		break;
	case 3: {
		int k;

		for (k = 0; k < AGNIC_PPORT_HDR_LEN; k++)
			d[AGNIC_PPORT_TAG_LEN + k] = (u8)(0xC0 + k);
		break;
	}
	case 0:
	default:
		memset(d + AGNIC_PPORT_TAG_LEN, 0, AGNIC_PPORT_HDR_LEN);
		break;
	}

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += frame_len;
	return agnic_giu_tx(ag, skb);
}

static const struct net_device_ops agnic_pport_ops = {
	.ndo_open		= agnic_pport_open,
	.ndo_stop		= agnic_pport_stop,
	.ndo_start_xmit		= agnic_pport_xmit,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};

/* ---- ethtool (observability) ------------------------------------------------ */

static void agnic_get_drvinfo(struct net_device *ndev, struct ethtool_drvinfo *info)
{
	struct agnic_port *p = netdev_priv(ndev);

	strscpy(info->driver, AGNIC_DRV_NAME, sizeof(info->driver));
	strscpy(info->version, AGNIC_DRV_VERSION, sizeof(info->version));
	strscpy(info->bus_info, pci_name(p->ag->pdev), sizeof(info->bus_info));
}

static void agnic_get_ringparam(struct net_device *ndev, struct ethtool_ringparam *rp,
				struct kernel_ethtool_ringparam *krp, struct netlink_ext_ack *ack)
{
	struct agnic_port *p = netdev_priv(ndev);

	rp->rx_max_pending = rp->rx_pending = p->ag->rx_ring.count;
	rp->tx_max_pending = rp->tx_pending = p->ag->tx_ring.count;
}

/* Aggregate GIU-trunk counters (shared by all front ports; per-port packet/byte
 * stats are the normal netdev stats). tx_drops climbing => TX ring full / device
 * not draining; rx_dropped => malformed/undeliverable frames off the demux. */
static const char agnic_gstrings[][ETH_GSTRING_LEN] = {
	"trunk_rx_frames", "trunk_rx_dropped", "trunk_tx_frames", "trunk_tx_drops",
	"rx_msix_irqs",
};

static int agnic_get_sset_count(struct net_device *ndev, int sset)
{
	return (sset == ETH_SS_STATS) ? (int)ARRAY_SIZE(agnic_gstrings) : -EOPNOTSUPP;
}

static void agnic_get_strings(struct net_device *ndev, u32 sset, u8 *data)
{
	if (sset == ETH_SS_STATS)
		memcpy(data, agnic_gstrings, sizeof(agnic_gstrings));
}

static void agnic_get_ethtool_stats(struct net_device *ndev, struct ethtool_stats *st, u64 *data)
{
	struct agnic *ag = ((struct agnic_port *)netdev_priv(ndev))->ag;

	data[0] = ag->rx_frames;
	data[1] = ag->rx_dropped;
	data[2] = ag->tx_frames;
	data[3] = ag->tx_drops;
	data[4] = (ag->msix_nvec > AGNIC_RX_DBELL_ID) ? ag->dbell[AGNIC_RX_DBELL_ID].count : 0;
}

static const struct ethtool_ops agnic_ethtool_ops = {
	.get_drvinfo		= agnic_get_drvinfo,
	.get_ringparam		= agnic_get_ringparam,
	.get_link		= ethtool_op_get_link,
	.get_sset_count		= agnic_get_sset_count,
	.get_strings		= agnic_get_strings,
	.get_ethtool_stats	= agnic_get_ethtool_stats,
};

/* The GIU-trunk aggregate counters, ALSO exposed via sysfs — the same values as the
 * ethtool GSTATS above, but readable on an image that ships no ethtool binary (D56). These are
 * the ONLY honest home for the demux drop count: it is PRE-demux, so it belongs to no front port
 * (attributing it to one would be a lie), and the netdev's own stats.rx_dropped is deliberately
 * left untouched. The group is attached to every front netdev because the counters are uniform
 * across the shared trunk; a reader (apid) takes them from any one port. Read-only. */
#define AGNIC_TRUNK_SHOW(field)                                          \
	static ssize_t field##_show(struct device *d,                          \
				    struct device_attribute *a, char *buf)     \
	{                                                                      \
		struct net_device *ndev = to_net_dev(d);                        \
		struct agnic_port *pp = netdev_priv(ndev);                      \
		return sysfs_emit(buf, "%llu\n", pp->ag->field);             \
	}                                                                      \
	static DEVICE_ATTR_RO(field)

AGNIC_TRUNK_SHOW(rx_frames);
AGNIC_TRUNK_SHOW(rx_dropped);
AGNIC_TRUNK_SHOW(tx_frames);
AGNIC_TRUNK_SHOW(tx_drops);

static struct attribute *agnic_trunk_attrs[] = {
	&dev_attr_rx_frames.attr,
	&dev_attr_rx_dropped.attr,
	&dev_attr_tx_frames.attr,
	&dev_attr_tx_drops.attr,
	NULL,
};

static const struct attribute_group agnic_trunk_group = {
	.name = "agnic_trunk",
	.attrs = agnic_trunk_attrs,
};

/* RX demux: route one trunk frame to its source front-panel netdev, strip the prefix.
 * skb->data points at the tag byte0 (the reaper already trimmed headroom+pkt_offset). */
void agnic_pport_rx(struct agnic *ag, struct sk_buff *skb)
{
	struct net_device *ndev;
	int port;

	if (skb->len < AGNIC_PPORT_PREFIX_LEN + ETH_HLEN)
		goto drop;

	/* Snapshot the first full 64-byte header once (for tx_hdr_mode=1 replay). */
	if (!ag->rx_last_hdr_valid) {
		memcpy(ag->rx_last_hdr, skb->data + AGNIC_PPORT_TAG_LEN, AGNIC_PPORT_HDR_LEN);
		ag->rx_last_hdr_valid = true;
	}

	port = AGNIC_PPORT_IDX_FROM_TAG0(skb->data[0]);	/* byte0 - 0x81 */
	if (port < 0 || port >= AGNIC_PPORT_COUNT || !ag->ports[port])
		goto drop;
	ndev = ag->ports[port];

	skb_pull(skb, AGNIC_PPORT_PREFIX_LEN);		/* strip tag + 64B header */
	skb->protocol = eth_type_trans(skb, ndev);
	ndev->stats.rx_packets++;
	ndev->stats.rx_bytes += skb->len;
	netif_rx(skb);
	return;
drop:
	ag->rx_dropped++;
	dev_kfree_skb_any(skb);
}

int agnic_pport_bringup(struct agnic *ag)
{
	u8 base[ETH_ALEN];
	int i, ret;

	/* Per-unit base, so two appliances on one L2 segment do not collide. Falls back
	 * to the historical fixed 02:81:00:00:00:00 when no per-unit source is readable. */
	agnic_port_mac_base(ag, base);

	for (i = 0; i < AGNIC_PPORT_COUNT; i++) {
		u8 mac[ETH_ALEN];
		struct net_device *ndev;
		struct agnic_port *p;

		memcpy(mac, base, ETH_ALEN);	/* not ether_addr_copy: no u16 alignment here */
		mac[5] = (u8)(i + 1);		/* distinct per port: 1..9 */

		ndev = alloc_etherdev(sizeof(struct agnic_port));
		if (!ndev) {
			ret = -ENOMEM;
			goto err;
		}
		SET_NETDEV_DEV(ndev, ag->dev);
		p = netdev_priv(ndev);
		p->ag = ag;
		p->index = i;
		eth_hw_addr_set(ndev, mac);
		ndev->netdev_ops = &agnic_pport_ops;
		ndev->ethtool_ops = &agnic_ethtool_ops;
		/* The trunk-aggregate counters as sysfs, readable without ethtool. Set BEFORE
		 * register_netdev so the group is created atomically with the device. */
		ndev->sysfs_groups[0] = &agnic_trunk_group;
		ndev->mtu = ETH_DATA_LEN;		/* 1500 */
		snprintf(ndev->name, IFNAMSIZ, "port%d", i + 1);

		ret = register_netdev(ndev);
		if (ret) {
			free_netdev(ndev);
			goto err;
		}
		/* Born UP: the generic NPU firmware exposes no usable PHY state, so
		 * assert carrier unconditionally (never demote on an unverified read). */
		netif_carrier_on(ndev);
		ag->ports[i] = ndev;
	}
	dev_info(ag->dev, "P4: %d front-panel netdevs registered (port1..port%d, born UP)\n",
		 AGNIC_PPORT_COUNT, AGNIC_PPORT_COUNT);
	return 0;
err:
	agnic_pport_teardown(ag);
	return ret;
}

void agnic_pport_teardown(struct agnic *ag)
{
	int i;

	for (i = 0; i < AGNIC_PPORT_COUNT; i++) {
		if (ag->ports[i]) {
			unregister_netdev(ag->ports[i]);
			free_netdev(ag->ports[i]);
			ag->ports[i] = NULL;
		}
	}
}
