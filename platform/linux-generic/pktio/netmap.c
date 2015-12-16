/* Copyright (c) 2015, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

#ifdef ODP_NETMAP

#include <odp_posix_extensions.h>

#include <odp_packet_io_internal.h>
#include <odp_packet_netmap.h>
#include <odp_packet_socket.h>
#include <odp_debug_internal.h>
#include <odp/helper/eth.h>

#include <sys/ioctl.h>
#include <poll.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <odp_classification_datamodel.h>
#include <odp_classification_inlines.h>
#include <odp_classification_internal.h>

#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>

#define NM_OPEN_RETRIES 5
#define NM_INJECT_RETRIES 10

static int netmap_do_ioctl(pktio_entry_t *pktio_entry, unsigned long cmd,
			   int subcmd)
{
	pkt_netmap_t *pkt_nm = &pktio_entry->s.pkt_nm;
	struct ethtool_value eval;
	struct ifreq ifr;
	int err;
	int fd = pkt_nm->sockfd;

	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s",
		 pktio_entry->s.name);

	switch (cmd) {
	case SIOCSIFFLAGS:
		ifr.ifr_flags = pkt_nm->if_flags & 0xffff;
		break;
	case SIOCETHTOOL:
		eval.cmd = subcmd;
		eval.data = 0;
		ifr.ifr_data = (caddr_t)&eval;
		break;
	default:
		break;
	}
	err = ioctl(fd, cmd, &ifr);
	if (err)
		goto done;

	switch (cmd) {
	case SIOCGIFFLAGS:
		pkt_nm->if_flags = (ifr.ifr_flags << 16) |
			(0xffff & ifr.ifr_flags);
		break;
	case SIOCETHTOOL:
		if (subcmd == ETHTOOL_GLINK)
			return eval.data;
		break;
	default:
		break;
	}
done:
	if (err)
		ODP_ERR("ioctl err %d %lu: %s\n", err, cmd, strerror(errno));

	return err;
}

/**
 * Map netmap rings to pktin/pktout queues
 *
 * @param rings          Array of netmap descriptor rings
 * @param num_queues     Number of pktin/pktout queues
 * @param num_rings      Number of matching netmap rings
 */
static inline void map_netmap_rings(netmap_ring_t *rings,
				    unsigned num_queues, unsigned num_rings)
{
	struct netmap_ring_t *desc_ring;
	unsigned rings_per_queue;
	unsigned remainder;
	unsigned mapped_rings;
	unsigned i;
	unsigned desc_id = 0;

	rings_per_queue = num_rings / num_queues;
	remainder = num_rings % num_queues;

	if (remainder)
		ODP_DBG("WARNING: Netmap rings mapped unevenly to queues\n");

	for (i = 0; i < num_queues; i++) {
		desc_ring = &rings[i].s;
		if (i < remainder)
			mapped_rings = rings_per_queue + 1;
		else
			mapped_rings = rings_per_queue;

		desc_ring->first = desc_id;
		desc_ring->cur = desc_id;
		desc_ring->last = desc_ring->first + mapped_rings - 1;
		desc_ring->num = mapped_rings;

		desc_id = desc_ring->last + 1;
	}
}

/**
 * Close pktio queues
 *
 * @param pktio_entry    Packet IO handle
 */
static inline void netmap_close_queues(pktio_entry_t *pktio_entry)
{
	int i;
	struct pktio_entry *pktio = &pktio_entry->s;
	odp_pktio_input_mode_t mode = pktio_entry->s.param.in_mode;

	for (i = 0; i < PKTIO_MAX_QUEUES; i++) {
		if (mode != ODP_PKTIN_MODE_POLL && mode != ODP_PKTIN_MODE_SCHED)
			continue;

		if (pktio->in_queue[i].queue != ODP_QUEUE_INVALID) {
			odp_queue_destroy(pktio->in_queue[i].queue);
			pktio->in_queue[i].queue = ODP_QUEUE_INVALID;
		}
	}
}

static int netmap_input_queues_config(pktio_entry_t *pktio_entry,
				      const odp_pktio_input_queue_param_t *p)
{
	struct pktio_entry *pktio = &pktio_entry->s;
	pkt_netmap_t *pkt_nm = &pktio_entry->s.pkt_nm;
	odp_pktio_input_mode_t mode = pktio_entry->s.param.in_mode;
	odp_queue_t queue;
	unsigned num_queues = p->num_queues;
	unsigned i;

	if (mode == ODP_PKTIN_MODE_DISABLED)
		return -1;

	if (num_queues <= 0 || num_queues > pkt_nm->capa.max_input_queues) {
		ODP_ERR("Invalid input queue count: %u\n", num_queues);
		return -1;
	}

	if (p->hash_enable && num_queues > 1) {
		if (rss_conf_set_fd(pktio_entry->s.pkt_nm.sockfd,
				    pktio_entry->s.name, &p->hash_proto)) {
			ODP_ERR("Failed to configure input hash\n");
			return -1;
		}
	}

	netmap_close_queues(pktio_entry);

	for (i = 0; i < num_queues; i++) {
		if (mode == ODP_PKTIN_MODE_POLL ||
		    mode == ODP_PKTIN_MODE_SCHED) {
			odp_queue_type_t type = ODP_QUEUE_TYPE_POLL;

			if (mode == ODP_PKTIN_MODE_SCHED)
				type = ODP_QUEUE_TYPE_SCHED;

			/* Ugly cast to uintptr_t is needed since queue_param
			 * is not defined as const in odp_queue_create() */
			queue = odp_queue_create("pktio_in", type,
						 (odp_queue_param_t *)
						 (uintptr_t)&p->queue_param);
			if (queue == ODP_QUEUE_INVALID) {
				netmap_close_queues(pktio_entry);
				return -1;
			}
			pktio->in_queue[i].queue = queue;
		} else {
			pktio->in_queue[i].queue = ODP_QUEUE_INVALID;
			pktio->in_queue[i].pktin.index = i;
			pktio->in_queue[i].pktin.pktio = pktio_entry->s.handle;
		}
	}
	/* Map pktin queues to netmap rings */
	map_netmap_rings(pkt_nm->rx_desc_ring, num_queues,
			 pkt_nm->num_rx_rings);

	pkt_nm->lockless_rx = p->single_user;
	pkt_nm->num_rx_queues = num_queues;
	return 0;
}

static int netmap_output_queues_config(pktio_entry_t *pktio_entry,
				       const odp_pktio_output_queue_param_t *p)
{
	struct pktio_entry *pktio = &pktio_entry->s;
	pkt_netmap_t *pkt_nm = &pktio_entry->s.pkt_nm;
	odp_pktio_output_mode_t mode = pktio_entry->s.param.out_mode;
	unsigned num_queues = p->num_queues;
	unsigned i;

	if (mode == ODP_PKTOUT_MODE_DISABLED)
		return -1;

	if (num_queues <= 0 || num_queues > pkt_nm->capa.max_output_queues) {
		ODP_ERR("Invalid output queue count: %u\n", num_queues);
		return -1;
	}

	/* Enough to map only one netmap tx ring per pktout queue */
	map_netmap_rings(pkt_nm->tx_desc_ring, num_queues, num_queues);

	for (i = 0; i < num_queues; i++) {
		pktio->out_queue[i].pktout.index = i;
		pktio->out_queue[i].pktout.pktio = pktio_entry->s.handle;
	}
	pkt_nm->lockless_tx = p->single_user;
	pkt_nm->num_tx_queues = num_queues;
	return 0;
}

static int netmap_close(pktio_entry_t *pktio_entry)
{
	int i, j;
	pkt_netmap_t *pkt_nm = &pktio_entry->s.pkt_nm;

	for (i = 0; i < PKTIO_MAX_QUEUES; i++) {
		for (j = 0; j < NM_MAX_DESC; j++) {
			if (pkt_nm->rx_desc_ring[i].s.desc[j] != NULL) {
				nm_close(pkt_nm->rx_desc_ring[i].s.desc[j]);
				pkt_nm->rx_desc_ring[i].s.desc[j] = NULL;
			}
		}
		for (j = 0; j < NM_MAX_DESC; j++) {
			if (pkt_nm->tx_desc_ring[i].s.desc[j] != NULL) {
				nm_close(pkt_nm->tx_desc_ring[i].s.desc[j]);
				pkt_nm->tx_desc_ring[i].s.desc[j] = NULL;
			}
		}
	}

	netmap_close_queues(pktio_entry);

	if (pkt_nm->sockfd != -1 && close(pkt_nm->sockfd) != 0) {
		__odp_errno = errno;
		ODP_ERR("close(sockfd): %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

/**
 * Determine netmap link status
 *
 * @param pktio_entry    Packet IO handle
 *
 * @retval  1 link is up
 * @retval  0 link is down
 * @retval <0 on failure
 */
static inline int netmap_link_status(pktio_entry_t *pktio_entry)
{
	int i;
	int ret;

	/* Wait for the link to come up */
	for (i = 0; i < NM_OPEN_RETRIES; i++) {
		ret = netmap_do_ioctl(pktio_entry, SIOCETHTOOL, ETHTOOL_GLINK);
		if (ret == -1)
			return -1;
		/* nm_open() causes the physical link to reset. When using a
		 * direct attached loopback cable there may be a small delay
		 * until the opposing end's interface comes back up again. In
		 * this case without the additional sleep pktio validation
		 * tests fail. */
		sleep(1);
		if (ret == 1)
			return 1;
	}
	ODP_DBG("%s link is down\n", pktio_entry->s.name);
	return 0;
}

static int netmap_open(odp_pktio_t id ODP_UNUSED, pktio_entry_t *pktio_entry,
		       const char *netdev, odp_pool_t pool)
{
	int i;
	int err;
	int sockfd;
	pkt_netmap_t *pkt_nm = &pktio_entry->s.pkt_nm;
	struct nm_desc *desc;
	odp_pktin_hash_proto_t hash_proto;

	if (getenv("ODP_PKTIO_DISABLE_NETMAP"))
		return -1;

	if (pool == ODP_POOL_INVALID)
		return -1;

	/* Init pktio entry */
	memset(pkt_nm, 0, sizeof(*pkt_nm));
	pkt_nm->sockfd = -1;
	pkt_nm->pool = pool;

	/* max frame len taking into account the l2-offset */
	pkt_nm->max_frame_len = ODP_CONFIG_PACKET_BUF_LEN_MAX -
		odp_buffer_pool_headroom(pool) -
		odp_buffer_pool_tailroom(pool);

	snprintf(pktio_entry->s.name, sizeof(pktio_entry->s.name), "%s",
		 netdev);
	snprintf(pkt_nm->nm_name, sizeof(pkt_nm->nm_name), "netmap:%s",
		 netdev);

	/* Dummy open here to check if netmap module is available and to read
	 * capability info. */
	desc = nm_open(pkt_nm->nm_name, NULL, 0, NULL);
	if (desc == NULL) {
		ODP_ERR("nm_open(%s) failed\n", pkt_nm->nm_name);
		goto error;
	}
	if (desc->nifp->ni_rx_rings > NM_MAX_DESC) {
		ODP_ERR("Unable to store all rx rings\n");
		nm_close(desc);
		goto error;
	}
	pkt_nm->num_rx_rings = desc->nifp->ni_rx_rings;
	pkt_nm->capa.max_input_queues = PKTIO_MAX_QUEUES;
	if (desc->nifp->ni_rx_rings < PKTIO_MAX_QUEUES)
		pkt_nm->capa.max_input_queues = desc->nifp->ni_rx_rings;

	if (desc->nifp->ni_tx_rings > NM_MAX_DESC) {
		ODP_ERR("Unable to store all tx rings\n");
		nm_close(desc);
		goto error;
	}
	pkt_nm->num_tx_rings = desc->nifp->ni_tx_rings;
	pkt_nm->capa.max_output_queues = PKTIO_MAX_QUEUES;
	if (desc->nifp->ni_tx_rings < PKTIO_MAX_QUEUES)
		pkt_nm->capa.max_output_queues = desc->nifp->ni_tx_rings;

	nm_close(desc);

	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd == -1) {
		ODP_ERR("Cannot get device control socket\n");
		goto error;
	}
	pkt_nm->sockfd = sockfd;

	/* Check if RSS is supported. If not, set 'max_input_queues' to 1. */
	if (rss_conf_get_supported_fd(sockfd, netdev, &hash_proto) == 0) {
		ODP_DBG("RSS not supported\n");
		pkt_nm->capa.max_input_queues = 1;
	}

	err = netmap_do_ioctl(pktio_entry, SIOCGIFFLAGS, 0);
	if (err)
		goto error;
	if ((pkt_nm->if_flags & IFF_UP) == 0)
		ODP_DBG("%s is down\n", pktio_entry->s.name);

	err = mac_addr_get_fd(sockfd, netdev, pkt_nm->if_mac);
	if (err)
		goto error;

	for (i = 0; i < PKTIO_MAX_QUEUES; i++) {
		odp_ticketlock_init(&pkt_nm->rx_desc_ring[i].s.lock);
		odp_ticketlock_init(&pkt_nm->tx_desc_ring[i].s.lock);
	}

	return 0;

error:
	netmap_close(pktio_entry);
	return -1;
}

static int netmap_start(pktio_entry_t *pktio_entry)
{
	pkt_netmap_t *pkt_nm = &pktio_entry->s.pkt_nm;
	netmap_ring_t *desc_ring;
	struct nm_desc base_desc;
	unsigned i;
	unsigned j;
	uint64_t flags;
	odp_pktio_input_mode_t in_mode = pktio_entry->s.param.in_mode;
	odp_pktio_output_mode_t out_mode = pktio_entry->s.param.out_mode;

	/* If no pktin/pktout queues have been configured. Configure one
	 * for each direction. */
	if (!pkt_nm->num_rx_queues && in_mode != ODP_PKTIN_MODE_DISABLED) {
		odp_pktio_input_queue_param_t param;

		memset(&param, 0, sizeof(odp_pktio_input_queue_param_t));
		param.num_queues = 1;
		if (netmap_input_queues_config(pktio_entry, &param))
			return -1;
	}
	if (!pkt_nm->num_tx_queues && out_mode == ODP_PKTOUT_MODE_SEND) {
		odp_pktio_output_queue_param_t param;

		memset(&param, 0, sizeof(odp_pktio_output_queue_param_t));
		param.num_queues = 1;
		if (netmap_output_queues_config(pktio_entry, &param))
			return -1;
	}

	base_desc.self = &base_desc;
	base_desc.mem = NULL;
	memcpy(base_desc.req.nr_name, pktio_entry->s.name,
	       sizeof(pktio_entry->s.name));
	base_desc.req.nr_flags &= ~NR_REG_MASK;
	base_desc.req.nr_flags |= NR_REG_ONE_NIC;
	base_desc.req.nr_ringid = 0;

	/* Only the first rx descriptor does mmap */
	desc_ring = pkt_nm->rx_desc_ring;
	flags = NM_OPEN_IFNAME | NETMAP_NO_TX_POLL;
	desc_ring[0].s.desc[0] = nm_open(pkt_nm->nm_name, NULL, flags,
					 &base_desc);
	if (desc_ring[0].s.desc[0] == NULL) {
		ODP_ERR("nm_start(%s) failed\n", pkt_nm->nm_name);
		goto error;
	}
	/* Open rest of the rx descriptors (one per netmap ring) */
	flags = NM_OPEN_IFNAME | NETMAP_NO_TX_POLL | NM_OPEN_NO_MMAP;
	for (i = 0; i < pkt_nm->num_rx_queues; i++) {
		for (j = desc_ring[i].s.first; j <= desc_ring[i].s.last; j++) {
			if (i == 0 && j == 0) /* First already opened */
				continue;
			base_desc.req.nr_ringid = j;
			desc_ring[i].s.desc[j] = nm_open(pkt_nm->nm_name, NULL,
							 flags, &base_desc);
			if (desc_ring[i].s.desc[j] == NULL) {
				ODP_ERR("nm_start(%s) failed\n",
					pkt_nm->nm_name);
				goto error;
			}
		}
	}
	/* Open tx descriptors */
	desc_ring = pkt_nm->tx_desc_ring;
	flags = NM_OPEN_IFNAME | NM_OPEN_NO_MMAP;
	for (i = 0; i < pkt_nm->num_tx_queues; i++) {
		for (j = desc_ring[i].s.first; j <= desc_ring[i].s.last; j++) {
			base_desc.req.nr_ringid = j;
			desc_ring[i].s.desc[j] = nm_open(pkt_nm->nm_name, NULL,
							 flags, &base_desc);
			if (desc_ring[i].s.desc[j] == NULL) {
				ODP_ERR("nm_start(%s) failed\n",
					pkt_nm->nm_name);
				goto error;
			}
		}
	}
	/* Wait for the link to come up */
	return (netmap_link_status(pktio_entry) == 1) ? 0 : -1;

error:
	netmap_close(pktio_entry);
	return -1;
}

/**
 * Create ODP packet from netmap packet
 *
 * @param pktio_entry    Packet IO handle
 * @param pkt_out        Storage for new ODP packet handle
 * @param buf            Netmap buffer address
 * @param len            Netmap buffer length
 *
 * @retval 0 on success
 * @retval <0 on failure
 */
static inline int netmap_pkt_to_odp(pktio_entry_t *pktio_entry,
				    odp_packet_t *pkt_out, const char *buf,
				    uint16_t len)
{
	odp_packet_t pkt;
	int ret;

	if (odp_unlikely(len > pktio_entry->s.pkt_nm.max_frame_len)) {
		ODP_ERR("RX: frame too big %" PRIu16 " %zu!\n", len,
			pktio_entry->s.pkt_nm.max_frame_len);
		return -1;
	}

	if (odp_unlikely(len < ODPH_ETH_LEN_MIN)) {
		ODP_ERR("RX: Frame truncated: %" PRIu16 "\n", len);
		return -1;
	}

	if (pktio_cls_enabled(pktio_entry)) {
		ret = _odp_packet_cls_enq(pktio_entry, (const uint8_t *)buf,
					  len, pkt_out);
		if (ret)
			return 0;
		return -1;
	} else {
		odp_packet_hdr_t *pkt_hdr;

		pkt = packet_alloc(pktio_entry->s.pkt_nm.pool, len, 1);
		if (pkt == ODP_PACKET_INVALID)
			return -1;

		pkt_hdr = odp_packet_hdr(pkt);

		/* For now copy the data in the mbuf,
		   worry about zero-copy later */
		if (odp_packet_copydata_in(pkt, 0, len, buf) != 0) {
			odp_packet_free(pkt);
			return -1;
		}

		packet_parse_l2(pkt_hdr);

		pkt_hdr->input = pktio_entry->s.handle;

		*pkt_out = pkt;
	}

	return 0;
}

static int netmap_recv_queue(pktio_entry_t *pktio_entry, int index,
			     odp_packet_t pkt_table[], int num)
{
	char *buf;
	struct netmap_ring *ring;
	struct nm_desc *desc;
	pkt_netmap_t *pkt_nm = &pktio_entry->s.pkt_nm;
	unsigned first_desc_id = pkt_nm->rx_desc_ring[index].s.first;
	unsigned last_desc_id = pkt_nm->rx_desc_ring[index].s.last;
	unsigned desc_id;
	int num_desc = pkt_nm->rx_desc_ring[index].s.num;
	int i;
	int num_rx = 0;
	int max_fd = 0;
	uint32_t slot_id;
	fd_set empty_rings;

	if (odp_unlikely(pktio_entry->s.state == STATE_STOP))
		return 0;

	FD_ZERO(&empty_rings);

	if (!pkt_nm->lockless_rx)
		odp_ticketlock_lock(&pkt_nm->rx_desc_ring[index].s.lock);

	desc_id = pkt_nm->rx_desc_ring[index].s.cur;

	for (i = 0; i < num_desc && num_rx != num; i++) {
		if (desc_id > last_desc_id)
			desc_id = first_desc_id;

		desc = pkt_nm->rx_desc_ring[index].s.desc[desc_id];
		ring = NETMAP_RXRING(desc->nifp, desc->cur_rx_ring);

		while (num_rx != num) {
			if (nm_ring_empty(ring)) {
				FD_SET(desc->fd, &empty_rings);
				if (desc->fd > max_fd)
					max_fd = desc->fd;
				break;
			}
			slot_id = ring->cur;
			buf = NETMAP_BUF(ring, ring->slot[slot_id].buf_idx);

			odp_prefetch(buf);

			if (!netmap_pkt_to_odp(pktio_entry, &pkt_table[num_rx],
					       buf, ring->slot[slot_id].len))
				num_rx++;

			ring->cur = nm_ring_next(ring, slot_id);
			ring->head = ring->cur;
		}
		desc_id++;
	}
	pkt_nm->rx_desc_ring[index].s.cur = desc_id;

	if (num_rx != num) {
		struct timeval tout = {.tv_sec = 0, .tv_usec = 0};

		if (select(max_fd + 1, &empty_rings, NULL, NULL, &tout) == -1)
			ODP_ERR("RX: select error\n");
	}
	if (!pkt_nm->lockless_rx)
		odp_ticketlock_unlock(&pkt_nm->rx_desc_ring[index].s.lock);

	return num_rx;
}

static int netmap_recv(pktio_entry_t *pktio_entry, odp_packet_t pkt_table[],
		       unsigned num)
{
	unsigned i;
	unsigned num_rx = 0;
	unsigned queue_id = pktio_entry->s.pkt_nm.cur_rx_queue;
	unsigned num_queues = pktio_entry->s.pkt_nm.num_rx_queues;
	unsigned pkts_left = num;
	odp_packet_t *pkt_table_cur = pkt_table;

	for (i = 0; i < num_queues && num_rx != num; i++) {
		if (queue_id >= num_queues)
			queue_id = 0;

		pkt_table_cur = &pkt_table[num_rx];
		pkts_left = num - num_rx;

		num_rx +=  netmap_recv_queue(pktio_entry, queue_id,
					     pkt_table_cur, pkts_left);
		queue_id++;
	}
	pktio_entry->s.pkt_nm.cur_rx_queue = queue_id;

	return num_rx;
}

static int netmap_send_queue(pktio_entry_t *pktio_entry, int index,
			     odp_packet_t pkt_table[], int num)
{
	pkt_netmap_t *pkt_nm = &pktio_entry->s.pkt_nm;
	struct pollfd polld;
	struct nm_desc *desc;
	struct netmap_ring *ring;
	int i;
	int nb_tx;
	int desc_id;
	odp_packet_t pkt;
	uint32_t pkt_len;
	unsigned slot_id;
	char *buf;

	if (odp_unlikely(pktio_entry->s.state == STATE_STOP))
		return 0;

	/* Only one netmap tx ring per pktout queue */
	desc_id = pkt_nm->tx_desc_ring[index].s.cur;
	desc = pkt_nm->tx_desc_ring[index].s.desc[desc_id];
	ring = NETMAP_TXRING(desc->nifp, desc->cur_tx_ring);

	if (!pkt_nm->lockless_tx)
		odp_ticketlock_lock(&pkt_nm->tx_desc_ring[index].s.lock);

	polld.fd = desc->fd;
	polld.events = POLLOUT;

	for (nb_tx = 0; nb_tx < num; nb_tx++) {
		pkt = pkt_table[nb_tx];
		pkt_len = odp_packet_len(pkt);

		if (pkt_len > ring->nr_buf_size) {
			if (nb_tx == 0)
				__odp_errno = EMSGSIZE;
			break;
		}
		for (i = 0; i < NM_INJECT_RETRIES; i++) {
			if (nm_ring_empty(ring)) {
				poll(&polld, 1, 0);
				continue;
			}
			slot_id = ring->cur;
			ring->slot[slot_id].flags = 0;
			ring->slot[slot_id].len = pkt_len;

			buf = NETMAP_BUF(ring, ring->slot[slot_id].buf_idx);

			if (odp_packet_copydata_out(pkt, 0, pkt_len, buf)) {
				i = NM_INJECT_RETRIES;
				break;
			}
			ring->cur = nm_ring_next(ring, slot_id);
			ring->head = ring->cur;
			break;
		}
		if (i == NM_INJECT_RETRIES)
			break;
		odp_packet_free(pkt);
	}
	/* Send pending packets */
	poll(&polld, 1, 0);

	if (!pkt_nm->lockless_tx)
		odp_ticketlock_unlock(&pkt_nm->tx_desc_ring[index].s.lock);

	if (odp_unlikely(nb_tx == 0 && __odp_errno != 0))
		return -1;

	return nb_tx;
}

static int netmap_send(pktio_entry_t *pktio_entry, odp_packet_t pkt_table[],
		       unsigned num)
{
	return netmap_send_queue(pktio_entry, 0, pkt_table, num);
}

static int netmap_mac_addr_get(pktio_entry_t *pktio_entry, void *mac_addr)
{
	memcpy(mac_addr, pktio_entry->s.pkt_nm.if_mac, ETH_ALEN);
	return ETH_ALEN;
}

static int netmap_mtu_get(pktio_entry_t *pktio_entry)
{
	return mtu_get_fd(pktio_entry->s.pkt_nm.sockfd, pktio_entry->s.name);
}

static int netmap_promisc_mode_set(pktio_entry_t *pktio_entry,
				   odp_bool_t enable)
{
	return promisc_mode_set_fd(pktio_entry->s.pkt_nm.sockfd,
				   pktio_entry->s.name, enable);
}

static int netmap_promisc_mode_get(pktio_entry_t *pktio_entry)
{
	return promisc_mode_get_fd(pktio_entry->s.pkt_nm.sockfd,
				   pktio_entry->s.name);
}

static int netmap_capability(pktio_entry_t *pktio_entry,
			     odp_pktio_capability_t *capa)
{
	*capa = pktio_entry->s.pkt_nm.capa;
	return 0;
}

static int netmap_in_queues(pktio_entry_t *pktio_entry, odp_queue_t queues[],
			    int num)
{
	int i;
	int num_rx_queues = pktio_entry->s.pkt_nm.num_rx_queues;

	if (queues && num > 0) {
		for (i = 0; i < num && i < num_rx_queues; i++)
			queues[i] = pktio_entry->s.in_queue[i].queue;
	}

	return pktio_entry->s.pkt_nm.num_rx_queues;
}

static int netmap_pktin_queues(pktio_entry_t *pktio_entry,
			       odp_pktin_queue_t queues[], int num)
{
	int i;
	int num_rx_queues = pktio_entry->s.pkt_nm.num_rx_queues;

	if (queues && num > 0) {
		for (i = 0; i < num && i < num_rx_queues; i++)
			queues[i] = pktio_entry->s.in_queue[i].pktin;
	}

	return pktio_entry->s.pkt_nm.num_rx_queues;
}

static int netmap_pktout_queues(pktio_entry_t *pktio_entry,
				odp_pktout_queue_t queues[], int num)
{
	int i;
	int num_tx_queues = pktio_entry->s.pkt_nm.num_tx_queues;

	if (queues && num > 0) {
		for (i = 0; i < num && i < num_tx_queues; i++)
			queues[i] = pktio_entry->s.out_queue[i].pktout;
	}

	return pktio_entry->s.pkt_nm.num_tx_queues;
}

const pktio_if_ops_t netmap_pktio_ops = {
	.init = NULL,
	.term = NULL,
	.open = netmap_open,
	.close = netmap_close,
	.start = netmap_start,
	.stop = NULL,
	.recv = netmap_recv,
	.send = netmap_send,
	.mtu_get = netmap_mtu_get,
	.promisc_mode_set = netmap_promisc_mode_set,
	.promisc_mode_get = netmap_promisc_mode_get,
	.mac_get = netmap_mac_addr_get,
	.capability = netmap_capability,
	.input_queues_config = netmap_input_queues_config,
	.output_queues_config = netmap_output_queues_config,
	.in_queues = netmap_in_queues,
	.pktin_queues = netmap_pktin_queues,
	.pktout_queues = netmap_pktout_queues,
	.recv_queue = netmap_recv_queue,
	.send_queue = netmap_send_queue
};

#endif /* ODP_NETMAP */
