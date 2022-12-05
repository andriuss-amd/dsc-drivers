// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2017 - 2022 Pensando Systems, Inc */

#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/if_vlan.h>
#include <net/ip6_checksum.h>
#include <linux/skbuff.h>

#include "ionic.h"
#include "ionic_lif.h"
#include "ionic_txrx.h"

static inline void ionic_txq_post(struct ionic_queue *q, bool ring_dbell,
				  ionic_desc_cb cb_func, void *cb_arg)
{
	DEBUG_STATS_TXQ_POST(q, ring_dbell);

	ionic_q_post(q, ring_dbell, cb_func, cb_arg);
}

static inline void ionic_rxq_post(struct ionic_queue *q, bool ring_dbell,
				  ionic_desc_cb cb_func, void *cb_arg)
{
	ionic_q_post(q, ring_dbell, cb_func, cb_arg);

	DEBUG_STATS_RX_BUFF_CNT(q);
}

bool ionic_txq_poke_doorbell(struct ionic_queue *q)
{
	unsigned long now, then, dif;
	struct netdev_queue *netdev_txq;
	struct net_device *netdev;

	netdev = q->lif->netdev;
	netdev_txq = netdev_get_tx_queue(netdev, q->index);

	HARD_TX_LOCK(netdev, netdev_txq, smp_processor_id());

	if (q->tail_idx == q->head_idx) {
		HARD_TX_UNLOCK(netdev, netdev_txq);
		return false;
	}

	now = READ_ONCE(jiffies);
	then = q->dbell_jiffies;
	dif = now - then;

	if (dif > q->dbell_deadline) {
		ionic_dbell_ring(q->lif->kern_dbpage, q->hw_type,
				 q->dbval | q->head_idx);

		q->dbell_jiffies = now;
	}

	HARD_TX_UNLOCK(netdev, netdev_txq);

	return true;
}

bool ionic_rxq_poke_doorbell(struct ionic_queue *q)
{
	unsigned long now, then, dif;

	/* no lock, called from rx napi or txrx napi, nothing else can fill */

	if (q->tail_idx == q->head_idx)
		return false;

	now = READ_ONCE(jiffies);
	then = q->dbell_jiffies;
	dif = now - then;

	if (dif > q->dbell_deadline) {
		ionic_dbell_ring(q->lif->kern_dbpage, q->hw_type,
				 q->dbval | q->head_idx);

		q->dbell_jiffies = now;

		dif = 2 * q->dbell_deadline;
		if (dif > IONIC_RX_MAX_DOORBELL_DEADLINE)
			dif = IONIC_RX_MAX_DOORBELL_DEADLINE;

		q->dbell_deadline = dif;
	}

	return true;
}

static inline struct netdev_queue *q_to_ndq(struct ionic_queue *q)
{
	return netdev_get_tx_queue(q->lif->netdev, q->index);
}

static inline int ionic_rx_page_alloc(struct ionic_queue *q,
				      struct ionic_buf_info *buf_info)
{
	struct net_device *netdev = q->lif->netdev;
	struct ionic_rx_stats *stats;
	struct device *dev;
	struct page *page;

	dev = q->dev;
	stats = q_to_rx_stats(q);

	if (unlikely(!buf_info)) {
		net_err_ratelimited("%s: %s invalid buf_info in alloc\n",
				    netdev->name, q->name);
		return -EINVAL;
	}

	page = alloc_pages(IONIC_PAGE_GFP_MASK, IONIC_PAGE_ORDER);
	if (unlikely(!page)) {
		net_err_ratelimited("%s: %s page alloc failed\n",
				    netdev->name, q->name);
		stats->alloc_err++;
		return -ENOMEM;
	}

	buf_info->dma_addr = dma_map_page(dev, page, 0,
					  IONIC_PAGE_SIZE, DMA_FROM_DEVICE);
	if (unlikely(dma_mapping_error(dev, buf_info->dma_addr))) {
		__free_pages(page, IONIC_PAGE_ORDER);
		net_err_ratelimited("%s: %s dma map failed\n",
				    netdev->name, q->name);
		stats->dma_map_err++;
		return -EIO;
	}

	buf_info->page = page;
	buf_info->page_offset = 0;
#if (IONIC_PAGE_ORDER > 0)
	buf_info->pagecnt_bias = 0;
#endif

	return 0;
}

static inline void ionic_rx_page_free(struct ionic_queue *q,
				      struct ionic_buf_info *buf_info)
{
	struct net_device *netdev = q->lif->netdev;
	struct device *dev = q->dev;

	if (unlikely(!buf_info)) {
		net_err_ratelimited("%s: %s invalid buf_info in free\n",
				    netdev->name, q->name);
		return;
	}

	if (!buf_info->page)
		return;

	dma_unmap_page(dev, buf_info->dma_addr, IONIC_PAGE_SIZE, DMA_FROM_DEVICE);
#if (IONIC_PAGE_ORDER > 0)
	if (buf_info->pagecnt_bias)
		page_ref_sub(buf_info->page, buf_info->pagecnt_bias);
#endif
	__free_pages(buf_info->page, IONIC_PAGE_ORDER);
	buf_info->page = NULL;
}

static bool ionic_rx_buf_recycle(struct ionic_queue *q,
				 struct ionic_buf_info *buf_info, u32 used)
{
	struct net_device *netdev = q->lif->netdev;
	u32 size;

	if (!dev_page_is_reusable(buf_info->page))
		return false;

	if (netdev->mtu > IONIC_PAGE_SPLIT_MAX_MTU)
		return false;

	size = ALIGN(used, IONIC_PAGE_SPLIT_SZ);
	buf_info->page_offset += size;
	if (buf_info->page_offset >= IONIC_PAGE_SIZE)
		return false;

#if (IONIC_PAGE_ORDER > 0)
	buf_info->pagecnt_bias--;
#else
	get_page(buf_info->page);
#endif
	return true;
}

static struct sk_buff *ionic_rx_frags(struct ionic_queue *q,
				      struct ionic_desc_info *desc_info,
				      struct ionic_rxq_comp *comp)
{
	struct net_device *netdev = q->lif->netdev;
	struct ionic_buf_info *buf_info;
	struct ionic_rx_stats *stats;
	struct device *dev = q->dev;
	struct sk_buff *skb;
	unsigned int i;
	u16 frag_len;
	u16 len;

	stats = q_to_rx_stats(q);

	buf_info = &desc_info->bufs[0];
	len = le16_to_cpu(comp->len);

	prefetchw(buf_info->page);

	skb = napi_get_frags(&q_to_qcq(q)->napi);
	if (unlikely(!skb)) {
		net_warn_ratelimited("%s: SKB alloc failed on %s!\n",
				     netdev->name, q->name);
		stats->alloc_err++;
		return NULL;
	}

	i = comp->num_sg_elems + 1;
	do {
		if (unlikely(!buf_info->page)) {
			dev_kfree_skb(skb);
			return NULL;
		}

		frag_len = min_t(u16, len, IONIC_PAGE_SIZE - buf_info->page_offset);
		len -= frag_len;

		dma_sync_single_for_cpu(dev,
					buf_info->dma_addr + buf_info->page_offset,
					frag_len, DMA_FROM_DEVICE);

		skb_add_rx_frag(skb, skb_shinfo(skb)->nr_frags,
				buf_info->page, buf_info->page_offset, frag_len,
				IONIC_PAGE_SIZE);

		if (!ionic_rx_buf_recycle(q, buf_info, frag_len)) {
			dma_unmap_page(dev, buf_info->dma_addr,
				       IONIC_PAGE_SIZE, DMA_FROM_DEVICE);
			buf_info->page = NULL;
		}

		buf_info++;

		i--;
	} while (i > 0);

	return skb;
}

static struct sk_buff *ionic_rx_copybreak(struct ionic_queue *q,
					  struct ionic_desc_info *desc_info,
					  struct ionic_rxq_comp *comp)
{
	struct net_device *netdev = q->lif->netdev;
	struct ionic_buf_info *buf_info;
	struct ionic_rx_stats *stats;
	struct device *dev = q->dev;
	struct sk_buff *skb;
	u16 len;

	stats = q_to_rx_stats(q);

	buf_info = &desc_info->bufs[0];
	len = le16_to_cpu(comp->len);

	skb = napi_alloc_skb(&q_to_qcq(q)->napi, len);
	if (unlikely(!skb)) {
		net_warn_ratelimited("%s: SKB alloc failed on %s!\n",
				     netdev->name, q->name);
		stats->alloc_err++;
		return NULL;
	}

	if (unlikely(!buf_info->page)) {
		dev_kfree_skb(skb);
		return NULL;
	}

	dma_sync_single_for_cpu(dev, buf_info->dma_addr + buf_info->page_offset,
				len, DMA_FROM_DEVICE);
	skb_copy_to_linear_data(skb, page_address(buf_info->page) + buf_info->page_offset, len);
	dma_sync_single_for_device(dev, buf_info->dma_addr + buf_info->page_offset,
				   len, DMA_FROM_DEVICE);

	skb_put(skb, len);
	skb->protocol = eth_type_trans(skb, q->lif->netdev);

	return skb;
}

static void ionic_rx_clean(struct ionic_queue *q,
			   struct ionic_desc_info *desc_info,
			   struct ionic_cq_info *cq_info,
			   void *cb_arg)
{
	struct net_device *netdev = q->lif->netdev;
	struct ionic_qcq *qcq = q_to_qcq(q);
	struct ionic_rx_stats *stats;
	struct ionic_rxq_comp *comp;
	struct sk_buff *skb;
#ifdef CSUM_DEBUG
	__sum16 csum;
#endif

	comp = cq_info->cq_desc + qcq->cq.desc_size - sizeof(*comp);

	stats = q_to_rx_stats(q);

	if (comp->status) {
		stats->dropped++;
		return;
	}

	if (le16_to_cpu(comp->len) > netdev->mtu + ETH_HLEN + VLAN_HLEN) {
		stats->dropped++;
		net_warn_ratelimited("%s: RX PKT TOO LARGE! comp->len %d\n",
				     netdev->name,
				     le16_to_cpu(comp->len));
		return;
	}

	stats->pkts++;
	stats->bytes += le16_to_cpu(comp->len);

	if (le16_to_cpu(comp->len) <= q->lif->rx_copybreak)
		skb = ionic_rx_copybreak(q, desc_info, comp);
	else
		skb = ionic_rx_frags(q, desc_info, comp);

	if (unlikely(!skb)) {
		stats->dropped++;
		return;
	}

#ifdef CSUM_DEBUG
	csum = ip_compute_csum(skb->data, skb->len);
#endif

	skb_record_rx_queue(skb, q->index);

	if (likely(netdev->features & NETIF_F_RXHASH)) {
		switch (comp->pkt_type_color & IONIC_RXQ_COMP_PKT_TYPE_MASK) {
		case IONIC_PKT_TYPE_IPV4:
		case IONIC_PKT_TYPE_IPV6:
			skb_set_hash(skb, le32_to_cpu(comp->rss_hash),
				     PKT_HASH_TYPE_L3);
			break;
		case IONIC_PKT_TYPE_IPV4_TCP:
		case IONIC_PKT_TYPE_IPV6_TCP:
		case IONIC_PKT_TYPE_IPV4_UDP:
		case IONIC_PKT_TYPE_IPV6_UDP:
			skb_set_hash(skb, le32_to_cpu(comp->rss_hash),
				     PKT_HASH_TYPE_L4);
			break;
		}
	}

	if (likely(netdev->features & NETIF_F_RXCSUM) &&
	    (comp->csum_flags & IONIC_RXQ_COMP_CSUM_F_CALC)) {
		skb->ip_summed = CHECKSUM_COMPLETE;
		skb->csum = (__force __wsum)le16_to_cpu(comp->csum);
#ifdef IONIC_DEBUG_STATS
		stats->csum_complete++;
#endif
#ifdef CSUM_DEBUG
		if (skb->csum != (u16)~csum)
			netdev_warn(netdev, "Rx CSUM incorrect. Want 0x%04x got 0x%04x, protocol 0x%04x\n",
				    (u16)~csum, skb->csum,
				    htons(skb->protocol));
#endif
	} else {
#ifdef IONIC_DEBUG_STATS
		stats->csum_none++;
#endif
	}

	if (unlikely((comp->csum_flags & IONIC_RXQ_COMP_CSUM_F_TCP_BAD) ||
		     (comp->csum_flags & IONIC_RXQ_COMP_CSUM_F_UDP_BAD) ||
		     (comp->csum_flags & IONIC_RXQ_COMP_CSUM_F_IP_BAD)))
		stats->csum_error++;

	if (likely(netdev->features & NETIF_F_HW_VLAN_CTAG_RX) &&
	    (comp->csum_flags & IONIC_RXQ_COMP_CSUM_F_VLAN)) {
		__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q),
				       le16_to_cpu(comp->vlan_tci));
#ifdef IONIC_DEBUG_STATS
		stats->vlan_stripped++;
#endif
	}

	if (unlikely(q->features & IONIC_RXQ_F_HWSTAMP)) {
		__le64 *cq_desc_hwstamp;
		u64 hwstamp;

		cq_desc_hwstamp =
			cq_info->cq_desc +
			qcq->cq.desc_size -
			sizeof(struct ionic_rxq_comp) -
			IONIC_HWSTAMP_CQ_NEGOFFSET;

		hwstamp = le64_to_cpu(*cq_desc_hwstamp);

		if (hwstamp != IONIC_HWSTAMP_INVALID) {
			skb_hwtstamps(skb)->hwtstamp = ionic_lif_phc_ktime(q->lif, hwstamp);
			stats->hwstamp_valid++;
		} else {
			stats->hwstamp_invalid++;
		}
	}

	if (le16_to_cpu(comp->len) <= q->lif->rx_copybreak)
		napi_gro_receive(&qcq->napi, skb);
	else
		napi_gro_frags(&qcq->napi);
}

bool ionic_rx_service(struct ionic_cq *cq, struct ionic_cq_info *cq_info)
{
	struct ionic_queue *q = cq->bound_q;
	struct ionic_desc_info *desc_info;
	struct ionic_rxq_comp *comp;

	comp = cq_info->cq_desc + cq->desc_size - sizeof(*comp);

	if (!color_match(comp->pkt_type_color, cq->done_color))
		return false;

	/* check for empty queue */
	if (q->tail_idx == q->head_idx)
		return false;

	if (q->tail_idx != le16_to_cpu(comp->comp_index))
		return false;

	desc_info = &q->info[q->tail_idx];
	q->tail_idx = (q->tail_idx + 1) & (q->num_descs - 1);

	/* clean the related q entry, only one per qc completion */
	ionic_rx_clean(q, desc_info, cq_info, desc_info->cb_arg);

	desc_info->cb = NULL;
	desc_info->cb_arg = NULL;

	return true;
}

void ionic_rx_fill(struct ionic_queue *q)
{
	struct net_device *netdev = q->lif->netdev;
	struct ionic_desc_info *desc_info;
	struct ionic_rxq_sg_desc *sg_desc;
	struct ionic_rxq_sg_elem *sg_elem;
	struct ionic_buf_info *buf_info;
	struct ionic_rxq_desc tmp_desc;
	unsigned int remain_len;
	unsigned int frag_len;
#if (IONIC_PAGE_ORDER > 0)
	unsigned int nsplits;
#endif
	unsigned int nfrags;
	unsigned int len;
	unsigned int i;
	unsigned int j;

	len = netdev->mtu + ETH_HLEN + VLAN_HLEN;
#if (IONIC_PAGE_ORDER > 0)
	nsplits = IONIC_PAGE_SIZE / ALIGN(len, IONIC_PAGE_SPLIT_SZ);
#endif

	for (i = ionic_q_space_avail(q); i; i--) {
		nfrags = 0;
		remain_len = len;
		desc_info = &q->info[q->head_idx];
		memset(&tmp_desc, 0, sizeof(tmp_desc));
		buf_info = &desc_info->bufs[0];

		if (!buf_info->page) { /* alloc a new buffer? */
			if (unlikely(ionic_rx_page_alloc(q, buf_info))) {
				tmp_desc.addr = 0;
				tmp_desc.len = 0;
				return;
			}
#if (IONIC_PAGE_ORDER > 0)
			buf_info->pagecnt_bias = nsplits - 1;
			if (buf_info->pagecnt_bias)
				page_ref_add(buf_info->page, buf_info->pagecnt_bias);
#endif
		}

		/* fill main descriptor - buf[0] */
		tmp_desc.addr = cpu_to_le64(buf_info->dma_addr + buf_info->page_offset);
		frag_len = min_t(u16, len, IONIC_PAGE_SIZE - buf_info->page_offset);
		tmp_desc.len = cpu_to_le16(frag_len);
		remain_len -= frag_len;
		buf_info++;
		nfrags++;

		/* fill sg descriptors - buf[1..n] */
		sg_desc = desc_info->sg_desc;
		for (j = 0; remain_len > 0 && j < q->max_sg_elems; j++) {
			sg_elem = &sg_desc->elems[j];
			if (!buf_info->page) { /* alloc a new sg buffer? */
				if (unlikely(ionic_rx_page_alloc(q, buf_info))) {
					sg_elem->addr = 0;
					sg_elem->len = 0;
					return;
				}
#if (IONIC_PAGE_ORDER > 0)
				buf_info->pagecnt_bias = nsplits - 1;
				if (buf_info->pagecnt_bias)
					page_ref_add(buf_info->page,
						     buf_info->pagecnt_bias);
#endif
			}

			sg_elem->addr = cpu_to_le64(buf_info->dma_addr + buf_info->page_offset);
			frag_len = min_t(u16, remain_len, IONIC_PAGE_SIZE - buf_info->page_offset);
			sg_elem->len = cpu_to_le16(frag_len);
			remain_len -= frag_len;
			buf_info++;
			nfrags++;
		}

		/* clear end sg element as a sentinel */
		if (j < q->max_sg_elems) {
			sg_elem = &sg_desc->elems[j];
			memset(sg_elem, 0, sizeof(*sg_elem));
		}

		tmp_desc.opcode = (nfrags > 1) ? IONIC_RXQ_DESC_OPCODE_SG :
						 IONIC_RXQ_DESC_OPCODE_SIMPLE;
		desc_info->nbufs = nfrags;

		/* commit descriptor contents in one shot */
		if (q_to_qcq(q)->flags & IONIC_QCQ_F_CMB_RINGS)
			memcpy_toio(desc_info->desc, &tmp_desc, sizeof(tmp_desc));
		else
			memcpy(desc_info->desc, &tmp_desc, sizeof(tmp_desc));

		ionic_rxq_post(q, false, ionic_rx_clean, NULL);
	}

	ionic_dbell_ring(q->lif->kern_dbpage, q->hw_type,
			 q->dbval | q->head_idx);

	q->dbell_deadline = IONIC_RX_MIN_DOORBELL_DEADLINE;
	q->dbell_jiffies = jiffies;

	mod_timer(&q_to_qcq(q)->napi_qcq->napi_deadline,
		  jiffies + IONIC_NAPI_DEADLINE);
}

void ionic_rx_empty(struct ionic_queue *q)
{
	struct ionic_desc_info *desc_info;
	struct ionic_buf_info *buf_info;
	unsigned int i, j;

	for (i = 0; i < q->num_descs; i++) {
		desc_info = &q->info[i];
		for (j = 0; j < IONIC_RX_MAX_SG_ELEMS + 1; j++) {
			buf_info = &desc_info->bufs[j];
			if (buf_info->page)
				ionic_rx_page_free(q, buf_info);
		}

		desc_info->nbufs = 0;
		desc_info->cb = NULL;
		desc_info->cb_arg = NULL;
	}

	q->head_idx = 0;
	q->tail_idx = 0;
}

static void ionic_dim_update(struct ionic_qcq *qcq, int napi_mode)
{
	struct dim_sample dim_sample;
	struct ionic_lif *lif;
	unsigned int qi;
	u64 pkts, bytes;

	if (!qcq->intr.dim_coal_hw)
		return;

	lif = qcq->q.lif;
	qi = qcq->cq.bound_q->index;

	switch (napi_mode) {
	case IONIC_LIF_F_TX_DIM_INTR:
		pkts = lif->txqstats[qi].pkts;
		bytes = lif->txqstats[qi].bytes;
		break;
	case IONIC_LIF_F_RX_DIM_INTR:
		pkts = lif->rxqstats[qi].pkts;
		bytes = lif->rxqstats[qi].bytes;
		break;
	default:
		pkts = lif->txqstats[qi].pkts + lif->rxqstats[qi].pkts;
		bytes = lif->txqstats[qi].bytes + lif->rxqstats[qi].bytes;
		break;
	}

	dim_update_sample_with_comps(qcq->cq.bound_intr->rearm_count,
				     pkts, bytes, 0, &dim_sample);

	net_dim(&qcq->dim, dim_sample);
}

int ionic_tx_napi(struct napi_struct *napi, int budget)
{
	struct ionic_qcq *qcq = napi_to_qcq(napi);
	struct ionic_cq *cq = napi_to_cq(napi);
	struct ionic_dev *idev;
	struct ionic_lif *lif;
	u32 work_done = 0;
	u32 flags = 0;
	u64 dbr;

	lif = cq->bound_q->lif;
	idev = &lif->ionic->idev;

	work_done = ionic_cq_service(cq, budget,
				     ionic_tx_service, NULL, NULL);

	if (work_done < budget && napi_complete_done(napi, work_done)) {
		flags |= IONIC_INTR_CRED_UNMASK;
		cq->bound_intr->rearm_count++;
	}

	if (work_done || flags) {
		flags |= IONIC_INTR_CRED_RESET_COALESCE;
		if (!lif->ionic->neth_eqs) {
			if (flags & IONIC_INTR_CRED_UNMASK)
				ionic_dim_update(qcq, IONIC_LIF_F_TX_DIM_INTR);
			ionic_intr_credits(idev->intr_ctrl,
					   cq->bound_intr->index,
					   work_done, flags);
		} else {
			if (!qcq->armed) {
				qcq->armed = true;
				dbr = IONIC_DBELL_RING_1 |
				      IONIC_DBELL_QID(qcq->q.hw_index);
				ionic_dbell_ring(lif->kern_dbpage,
						 qcq->q.hw_type,
						 dbr | qcq->cq.tail_idx);
			}
		}
	}

	if (!work_done && ionic_txq_poke_doorbell(&qcq->q))
		mod_timer(&qcq->napi_deadline, jiffies + IONIC_NAPI_DEADLINE);

	DEBUG_STATS_NAPI_POLL(qcq, work_done);

	return work_done;
}

int ionic_rx_napi(struct napi_struct *napi, int budget)
{
	struct ionic_qcq *qcq = napi_to_qcq(napi);
	struct ionic_cq *cq = napi_to_cq(napi);
	struct ionic_dev *idev;
	struct ionic_lif *lif;
	u16 rx_fill_threshold;
	u32 work_done = 0;
	u32 flags = 0;
	u64 dbr;

	lif = cq->bound_q->lif;
	idev = &lif->ionic->idev;

	work_done = ionic_cq_service(cq, budget,
				     ionic_rx_service, NULL, NULL);

	rx_fill_threshold = min_t(u16, IONIC_RX_FILL_THRESHOLD,
				  cq->num_descs / IONIC_RX_FILL_DIV);
	if (work_done && ionic_q_space_avail(cq->bound_q) >= rx_fill_threshold)
		ionic_rx_fill(cq->bound_q);

	if (work_done < budget && napi_complete_done(napi, work_done)) {
		flags |= IONIC_INTR_CRED_UNMASK;
		cq->bound_intr->rearm_count++;
	}

	if (work_done || flags) {
		flags |= IONIC_INTR_CRED_RESET_COALESCE;
		if (!lif->ionic->neth_eqs) {
			if (flags & IONIC_INTR_CRED_UNMASK)
				ionic_dim_update(qcq, IONIC_LIF_F_RX_DIM_INTR);
			ionic_intr_credits(idev->intr_ctrl,
					   cq->bound_intr->index,
					   work_done, flags);
		} else {
			if (!qcq->armed) {
				qcq->armed = true;
				dbr = IONIC_DBELL_RING_1 |
				      IONIC_DBELL_QID(qcq->q.hw_index);
				ionic_dbell_ring(lif->kern_dbpage,
						 qcq->q.hw_type,
						 dbr | qcq->cq.tail_idx);
			}
		}
	}

	if (!work_done && ionic_rxq_poke_doorbell(&qcq->q))
		mod_timer(&qcq->napi_deadline, jiffies + IONIC_NAPI_DEADLINE);

	DEBUG_STATS_NAPI_POLL(qcq, work_done);

	return work_done;
}

int ionic_txrx_napi(struct napi_struct *napi, int budget)
{
	struct ionic_qcq *rxqcq = napi_to_qcq(napi);
	struct ionic_cq *rxcq = napi_to_cq(napi);
	unsigned int qi = rxcq->bound_q->index;
	struct ionic_dev *idev;
	struct ionic_lif *lif;
	struct ionic_qcq *txqcq;
	struct ionic_cq *txcq;
	u16 rx_fill_threshold;
	bool resched = false;
	u32 tx_work_done = 0;
	u32 rx_work_done = 0;
	u32 flags = 0;

	lif = rxcq->bound_q->lif;
	idev = &lif->ionic->idev;
	txqcq = lif->txqcqs[qi];
	txcq = &lif->txqcqs[qi]->cq;

	tx_work_done = ionic_cq_service(txcq, tx_budget,
					ionic_tx_service, NULL, NULL);

	rx_work_done = ionic_cq_service(rxcq, budget,
					ionic_rx_service, NULL, NULL);

	rx_fill_threshold = min_t(u16, IONIC_RX_FILL_THRESHOLD,
				  rxcq->num_descs / IONIC_RX_FILL_DIV);
	if (rx_work_done && ionic_q_space_avail(rxcq->bound_q) >= rx_fill_threshold)
		ionic_rx_fill(rxcq->bound_q);

	if (rx_work_done < budget && napi_complete_done(napi, rx_work_done)) {
		flags |= IONIC_INTR_CRED_UNMASK;
		rxcq->bound_intr->rearm_count++;
	}

	if (rx_work_done || flags) {
		flags |= IONIC_INTR_CRED_RESET_COALESCE;
		if (!lif->ionic->neth_eqs) {
			if (flags & IONIC_INTR_CRED_UNMASK)
				ionic_dim_update(rxqcq, 0);
			ionic_intr_credits(idev->intr_ctrl,
					   rxcq->bound_intr->index,
					   tx_work_done + rx_work_done, flags);
		} else {
			u64 dbr;

			if (!rxqcq->armed) {
				rxqcq->armed = true;
				dbr = IONIC_DBELL_RING_1 |
				      IONIC_DBELL_QID(rxqcq->q.hw_index);
				ionic_dbell_ring(lif->kern_dbpage,
						 rxqcq->q.hw_type,
						 dbr | rxqcq->cq.tail_idx);
			}
			if (!txqcq->armed) {
				txqcq->armed = true;
				dbr = IONIC_DBELL_RING_1 |
				      IONIC_DBELL_QID(txqcq->q.hw_index);
				ionic_dbell_ring(lif->kern_dbpage,
						 txqcq->q.hw_type,
						 dbr | txqcq->cq.tail_idx);
			}
		}
	}

	DEBUG_STATS_NAPI_POLL(rxqcq, rx_work_done);
	DEBUG_STATS_NAPI_POLL(txqcq, tx_work_done);

	if (!rx_work_done && ionic_rxq_poke_doorbell(&rxqcq->q))
		resched = true;
	if (!tx_work_done && ionic_txq_poke_doorbell(&txqcq->q))
		resched = true;
	if (resched)
		mod_timer(&rxqcq->napi_deadline, jiffies + IONIC_NAPI_DEADLINE);

	return rx_work_done;
}

static dma_addr_t ionic_tx_map_single(struct ionic_queue *q,
				      void *data, size_t len)
{
	struct ionic_tx_stats *stats = q_to_tx_stats(q);
	struct device *dev = q->dev;
	dma_addr_t dma_addr;

	dma_addr = dma_map_single(dev, data, len, DMA_TO_DEVICE);
	if (dma_mapping_error(dev, dma_addr)) {
		net_warn_ratelimited("%s: DMA single map failed on %s!\n",
				     q->lif->netdev->name, q->name);
		stats->dma_map_err++;
		return 0;
	}
	return dma_addr;
}

static dma_addr_t ionic_tx_map_frag(struct ionic_queue *q,
				    const skb_frag_t *frag,
				    size_t offset, size_t len)
{
	struct ionic_tx_stats *stats = q_to_tx_stats(q);
	struct device *dev = q->dev;
	dma_addr_t dma_addr;

	dma_addr = skb_frag_dma_map(dev, frag, offset, len, DMA_TO_DEVICE);
	if (dma_mapping_error(dev, dma_addr)) {
		net_warn_ratelimited("%s: DMA frag map failed on %s!\n",
				     q->lif->netdev->name, q->name);
		stats->dma_map_err++;
	}
	return dma_addr;
}

static int ionic_tx_map_skb(struct ionic_queue *q, struct sk_buff *skb,
			    struct ionic_desc_info *desc_info)
{
	struct ionic_buf_info *buf_info = desc_info->bufs;
	struct ionic_tx_stats *stats = q_to_tx_stats(q);
	struct device *dev = q->dev;
	dma_addr_t dma_addr;
	unsigned int nfrags;
	skb_frag_t *frag;
	int frag_idx;

	dma_addr = ionic_tx_map_single(q, skb->data, skb_headlen(skb));
	if (dma_mapping_error(dev, dma_addr)) {
		stats->dma_map_err++;
		return -EIO;
	}
	buf_info->dma_addr = dma_addr;
	buf_info->len = skb_headlen(skb);
	buf_info++;

	frag = skb_shinfo(skb)->frags;
	nfrags = skb_shinfo(skb)->nr_frags;
	for (frag_idx = 0; frag_idx < nfrags; frag_idx++, frag++) {
		dma_addr = ionic_tx_map_frag(q, frag, 0, skb_frag_size(frag));
		if (dma_mapping_error(dev, dma_addr)) {
			stats->dma_map_err++;
			goto dma_fail;
		}
		buf_info->dma_addr = dma_addr;
		buf_info->len = skb_frag_size(frag);
		buf_info++;
	}

	desc_info->nbufs = 1 + nfrags;

	return 0;

dma_fail:
	/* unwind the frag mappings and the head mapping */
	while (frag_idx > 0) {
		frag_idx--;
		buf_info--;
		dma_unmap_page(dev, buf_info->dma_addr,
			       buf_info->len, DMA_TO_DEVICE);
	}
	dma_unmap_single(dev, buf_info->dma_addr, buf_info->len, DMA_TO_DEVICE);
	return -EIO;
}

static void ionic_tx_desc_unmap_bufs(struct ionic_queue *q,
				     struct ionic_desc_info *desc_info)
{
	struct ionic_buf_info *buf_info = desc_info->bufs;
	struct device *dev = q->dev;
	unsigned int i;

	if (!desc_info->nbufs)
		return;

	dma_unmap_single(dev, (dma_addr_t)buf_info->dma_addr,
			 buf_info->len, DMA_TO_DEVICE);
	buf_info++;
	for (i = 1; i < desc_info->nbufs; i++, buf_info++)
		dma_unmap_page(dev, (dma_addr_t)buf_info->dma_addr,
			       buf_info->len, DMA_TO_DEVICE);

	desc_info->nbufs = 0;
}

static void ionic_tx_clean(struct ionic_queue *q,
			   struct ionic_desc_info *desc_info,
			   struct ionic_cq_info *cq_info,
			   void *cb_arg)
{
	struct ionic_tx_stats *stats = q_to_tx_stats(q);
	struct ionic_qcq *qcq = q_to_qcq(q);
	struct sk_buff *skb = cb_arg;
	u16 qi;

	ionic_tx_desc_unmap_bufs(q, desc_info);

	if (!skb)
		return;

	qi = skb_get_queue_mapping(skb);

	if (unlikely(q->features & IONIC_TXQ_F_HWSTAMP)) {
		if (cq_info) {
			struct skb_shared_hwtstamps hwts = {};
			__le64 *cq_desc_hwstamp;
			u64 hwstamp;

			cq_desc_hwstamp =
				cq_info->cq_desc +
				qcq->cq.desc_size -
				sizeof(struct ionic_txq_comp) -
				IONIC_HWSTAMP_CQ_NEGOFFSET;

			hwstamp = le64_to_cpu(*cq_desc_hwstamp);

			if (hwstamp != IONIC_HWSTAMP_INVALID) {
				hwts.hwtstamp = ionic_lif_phc_ktime(q->lif, hwstamp);

				skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
				skb_tstamp_tx(skb, &hwts);

				stats->hwstamp_valid++;
			} else {
				stats->hwstamp_invalid++;
			}
		}

	} else if (unlikely(__netif_subqueue_stopped(q->lif->netdev, qi))) {
		netif_wake_subqueue(q->lif->netdev, qi);
		q->wake++;
	}

	desc_info->bytes = skb->len;
	stats->clean++;

	dev_consume_skb_any(skb);
}

bool ionic_tx_service(struct ionic_cq *cq, struct ionic_cq_info *cq_info)
{
	struct ionic_queue *q = cq->bound_q;
	struct ionic_desc_info *desc_info;
	struct ionic_txq_comp *comp;
	int bytes = 0;
	int pkts = 0;
	u16 index;

	comp = cq_info->cq_desc + cq->desc_size - sizeof(*comp);

	if (!color_match(comp->color, cq->done_color))
		return false;

	/* clean the related q entries, there could be
	 * several q entries completed for each cq completion
	 */
	do {
		desc_info = &q->info[q->tail_idx];
		desc_info->bytes = 0;
		index = q->tail_idx;
		q->tail_idx = (q->tail_idx + 1) & (q->num_descs - 1);
		ionic_tx_clean(q, desc_info, cq_info, desc_info->cb_arg);
		if (desc_info->cb_arg) {
			pkts++;
			bytes += desc_info->bytes;
		}
		desc_info->cb = NULL;
		desc_info->cb_arg = NULL;
	} while (index != le16_to_cpu(comp->comp_index));

#ifdef IONIC_SUPPORTS_BQL
	if (pkts && bytes && !unlikely(q->features & IONIC_TXQ_F_HWSTAMP))
		netdev_tx_completed_queue(q_to_ndq(q), pkts, bytes);
#endif

	return true;
}

void ionic_tx_flush(struct ionic_cq *cq)
{
	struct ionic_dev *idev = &cq->lif->ionic->idev;
	u32 work_done;

	work_done = ionic_cq_service(cq, cq->num_descs,
				     ionic_tx_service, NULL, NULL);

	if (work_done && !cq->lif->ionic->neth_eqs)
		ionic_intr_credits(idev->intr_ctrl, cq->bound_intr->index,
				   work_done, IONIC_INTR_CRED_RESET_COALESCE);
}

void ionic_tx_empty(struct ionic_queue *q)
{
	struct ionic_desc_info *desc_info;
	int bytes = 0;
	int pkts = 0;

	/* walk the not completed tx entries, if any */
	while (q->head_idx != q->tail_idx) {
		desc_info = &q->info[q->tail_idx];
		desc_info->bytes = 0;
		q->tail_idx = (q->tail_idx + 1) & (q->num_descs - 1);
		ionic_tx_clean(q, desc_info, NULL, desc_info->cb_arg);
		if (desc_info->cb_arg) {
			pkts++;
			bytes += desc_info->bytes;
		}
		desc_info->cb = NULL;
		desc_info->cb_arg = NULL;
	}

#ifdef IONIC_SUPPORTS_BQL
	if (pkts && bytes && !unlikely(q->features & IONIC_TXQ_F_HWSTAMP))
		netdev_tx_completed_queue(q_to_ndq(q), pkts, bytes);
#endif
}

static int ionic_tx_tcp_inner_pseudo_csum(struct sk_buff *skb)
{
	int err;

	err = skb_cow_head(skb, 0);
	if (err)
		return err;

	if (skb->protocol == cpu_to_be16(ETH_P_IP)) {
		inner_ip_hdr(skb)->check = 0;
		inner_tcp_hdr(skb)->check =
			~csum_tcpudp_magic(inner_ip_hdr(skb)->saddr,
					   inner_ip_hdr(skb)->daddr,
					   0, IPPROTO_TCP, 0);
	} else if (skb->protocol == cpu_to_be16(ETH_P_IPV6)) {
		inner_tcp_hdr(skb)->check =
			~csum_ipv6_magic(&inner_ipv6_hdr(skb)->saddr,
					 &inner_ipv6_hdr(skb)->daddr,
					 0, IPPROTO_TCP, 0);
	}

	return 0;
}

static int ionic_tx_tcp_pseudo_csum(struct sk_buff *skb)
{
	int err;

	err = skb_cow_head(skb, 0);
	if (err)
		return err;

	if (skb->protocol == cpu_to_be16(ETH_P_IP)) {
		ip_hdr(skb)->check = 0;
		tcp_hdr(skb)->check =
			~csum_tcpudp_magic(ip_hdr(skb)->saddr,
					   ip_hdr(skb)->daddr,
					   0, IPPROTO_TCP, 0);
	} else if (skb->protocol == cpu_to_be16(ETH_P_IPV6)) {
		tcp_hdr(skb)->check =
			~csum_ipv6_magic(&ipv6_hdr(skb)->saddr,
					 &ipv6_hdr(skb)->daddr,
					 0, IPPROTO_TCP, 0);
	}

	return 0;
}

static void ionic_tx_tso_post(struct ionic_queue *q,
			      struct ionic_txq_desc *desc,
			      struct sk_buff *skb,
			      dma_addr_t addr, u8 nsge, u16 len,
			      unsigned int hdrlen, unsigned int mss,
			      bool outer_csum,
			      u16 vlan_tci, bool has_vlan,
			      bool start, bool done)
{
	struct ionic_txq_desc tmp_desc = {0};
	u8 flags = 0;
	u64 cmd;

	flags |= has_vlan ? IONIC_TXQ_DESC_FLAG_VLAN : 0;
	flags |= outer_csum ? IONIC_TXQ_DESC_FLAG_ENCAP : 0;
	flags |= start ? IONIC_TXQ_DESC_FLAG_TSO_SOT : 0;
	flags |= done ? IONIC_TXQ_DESC_FLAG_TSO_EOT : 0;

	cmd = encode_txq_desc_cmd(IONIC_TXQ_DESC_OPCODE_TSO, flags, nsge, addr);
	tmp_desc.cmd = cpu_to_le64(cmd);
	tmp_desc.len = cpu_to_le16(len);
	tmp_desc.vlan_tci = cpu_to_le16(vlan_tci);
	tmp_desc.hdr_len = cpu_to_le16(hdrlen);
	tmp_desc.mss = cpu_to_le16(mss);

	/* commit descriptor contents in one shot */
	if (q_to_qcq(q)->flags & IONIC_QCQ_F_CMB_RINGS)
		memcpy_toio(desc, &tmp_desc, sizeof(tmp_desc));
	else
		memcpy(desc, &tmp_desc, sizeof(tmp_desc));

	if (start) {
		skb_tx_timestamp(skb);
#ifdef IONIC_SUPPORTS_BQL
		if (!unlikely(q->features & IONIC_TXQ_F_HWSTAMP))
			netdev_tx_sent_queue(q_to_ndq(q), skb->len);
#endif
		ionic_txq_post(q, false, ionic_tx_clean, skb);
	} else {
		ionic_txq_post(q, done, NULL, NULL);
	}
}

static int ionic_tx_tso(struct ionic_queue *q, struct sk_buff *skb)
{
	struct ionic_tx_stats *stats = q_to_tx_stats(q);
	struct ionic_desc_info *desc_info;
	struct ionic_buf_info *buf_info;
	struct ionic_txq_sg_elem *elem;
	struct ionic_txq_desc *desc;
	unsigned int chunk_len;
	unsigned int frag_rem;
	unsigned int tso_rem;
	unsigned int seg_rem;
	dma_addr_t desc_addr;
	dma_addr_t frag_addr;
	unsigned int hdrlen;
	unsigned int len;
	unsigned int mss;
	bool start, done;
	bool outer_csum;
	bool has_vlan;
	u16 desc_len;
	u8 desc_nsge;
	u16 vlan_tci;
	bool encap;
	int err;

	desc_info = &q->info[q->head_idx];
	buf_info = desc_info->bufs;

	if (unlikely(ionic_tx_map_skb(q, skb, desc_info)))
		return -EIO;

	len = skb->len;
	mss = skb_shinfo(skb)->gso_size;
	outer_csum = (skb_shinfo(skb)->gso_type & SKB_GSO_GRE_CSUM) ||
		     (skb_shinfo(skb)->gso_type & SKB_GSO_UDP_TUNNEL_CSUM);
	has_vlan = !!skb_vlan_tag_present(skb);
	vlan_tci = skb_vlan_tag_get(skb);
	encap = skb->encapsulation;

	/* Preload inner-most TCP csum field with IP pseudo hdr
	 * calculated with IP length set to zero.  HW will later
	 * add in length to each TCP segment resulting from the TSO.
	 */

	if (encap)
		err = ionic_tx_tcp_inner_pseudo_csum(skb);
	else
		err = ionic_tx_tcp_pseudo_csum(skb);
	if (err) {
		/* clean up mapping from ionic_tx_map_skb */
		ionic_tx_desc_unmap_bufs(q, desc_info);
		return err;
	}

	if (encap)
		hdrlen = skb_inner_transport_header(skb) - skb->data +
			 inner_tcp_hdrlen(skb);
	else
		hdrlen = skb_transport_offset(skb) + tcp_hdrlen(skb);

	tso_rem = len;
	seg_rem = min(tso_rem, hdrlen + mss);

	frag_addr = 0;
	frag_rem = 0;

	start = true;

	while (tso_rem > 0) {
		desc = NULL;
		elem = NULL;
		desc_addr = 0;
		desc_len = 0;
		desc_nsge = 0;
		/* use fragments until we have enough to post a single descriptor */
		while (seg_rem > 0) {
			/* if the fragment is exhausted then move to the next one */
			if (frag_rem == 0) {
				/* grab the next fragment */
				frag_addr = buf_info->dma_addr;
				frag_rem = buf_info->len;
				buf_info++;
			}
			chunk_len = min(frag_rem, seg_rem);
			if (!desc) {
				/* fill main descriptor */
				desc = desc_info->txq_desc;
				elem = desc_info->txq_sg_desc->elems;
				desc_addr = frag_addr;
				desc_len = chunk_len;
			} else {
				/* fill sg descriptor */
				elem->addr = cpu_to_le64(frag_addr);
				elem->len = cpu_to_le16(chunk_len);
				elem++;
				desc_nsge++;
			}
			frag_addr += chunk_len;
			frag_rem -= chunk_len;
			tso_rem -= chunk_len;
			seg_rem -= chunk_len;
		}
		seg_rem = min(tso_rem, mss);
		done = (tso_rem == 0);
		/* post descriptor */
		ionic_tx_tso_post(q, desc, skb,
				  desc_addr, desc_nsge, desc_len,
				  hdrlen, mss, outer_csum, vlan_tci, has_vlan,
				  start, done);
		start = false;
		/* Buffer information is stored with the first tso descriptor */
		desc_info = &q->info[q->head_idx];
		desc_info->nbufs = 0;
	}

	stats->pkts += DIV_ROUND_UP(len - hdrlen, mss);
	stats->bytes += len;
	stats->tso++;
	stats->tso_bytes = len;

	return 0;
}

static void ionic_tx_calc_csum(struct ionic_queue *q, struct sk_buff *skb,
			       struct ionic_desc_info *desc_info)
{
	struct ionic_buf_info *buf_info = desc_info->bufs;
#ifdef IONIC_DEBUG_STATS
	struct ionic_tx_stats *stats = q_to_tx_stats(q);
#endif
	struct ionic_txq_desc tmp_desc = {0};
	bool has_vlan;
	u8 flags = 0;
	bool encap;
	u64 cmd;

	has_vlan = !!skb_vlan_tag_present(skb);
	encap = skb->encapsulation;

	flags |= has_vlan ? IONIC_TXQ_DESC_FLAG_VLAN : 0;
	flags |= encap ? IONIC_TXQ_DESC_FLAG_ENCAP : 0;

	cmd = encode_txq_desc_cmd(IONIC_TXQ_DESC_OPCODE_CSUM_PARTIAL,
				  flags, skb_shinfo(skb)->nr_frags,
				  buf_info->dma_addr);
	tmp_desc.cmd = cpu_to_le64(cmd);
	tmp_desc.len = cpu_to_le16(buf_info->len);
	if (has_vlan) {
		tmp_desc.vlan_tci = cpu_to_le16(skb_vlan_tag_get(skb));
#ifdef IONIC_DEBUG_STATS
		stats->vlan_inserted++;
#endif
	}
	tmp_desc.csum_start = cpu_to_le16(skb_checksum_start_offset(skb));
	tmp_desc.csum_offset = cpu_to_le16(skb->csum_offset);

	/* commit descriptor contents in one shot */
	if (q_to_qcq(q)->flags & IONIC_QCQ_F_CMB_RINGS)
		memcpy_toio(desc_info->desc, &tmp_desc, sizeof(tmp_desc));
	else
		memcpy(desc_info->desc, &tmp_desc, sizeof(tmp_desc));

#ifdef IONIC_DEBUG_STATS
#ifdef HAVE_CSUM_NOT_INET
	if (skb->csum_not_inet)
		stats->crc32_csum++;
	else
#endif
		stats->csum++;
#endif
}

static void ionic_tx_calc_no_csum(struct ionic_queue *q, struct sk_buff *skb,
				  struct ionic_desc_info *desc_info)
{
	struct ionic_buf_info *buf_info = desc_info->bufs;
#ifdef IONIC_DEBUG_STATS
	struct ionic_tx_stats *stats = q_to_tx_stats(q);
#endif
	struct ionic_txq_desc tmp_desc = {0};
	bool has_vlan;
	u8 flags = 0;
	bool encap;
	u64 cmd;

	has_vlan = !!skb_vlan_tag_present(skb);
	encap = skb->encapsulation;

	flags |= has_vlan ? IONIC_TXQ_DESC_FLAG_VLAN : 0;
	flags |= encap ? IONIC_TXQ_DESC_FLAG_ENCAP : 0;

	cmd = encode_txq_desc_cmd(IONIC_TXQ_DESC_OPCODE_CSUM_NONE,
				  flags, skb_shinfo(skb)->nr_frags,
				  buf_info->dma_addr);
	tmp_desc.cmd = cpu_to_le64(cmd);
	tmp_desc.len = cpu_to_le16(buf_info->len);
	if (has_vlan) {
		tmp_desc.vlan_tci = cpu_to_le16(skb_vlan_tag_get(skb));
#ifdef IONIC_DEBUG_STATS
		stats->vlan_inserted++;
#endif
	}

	/* commit descriptor contents in one shot */
	if (q_to_qcq(q)->flags & IONIC_QCQ_F_CMB_RINGS)
		memcpy_toio(desc_info->desc, &tmp_desc, sizeof(tmp_desc));
	else
		memcpy(desc_info->desc, &tmp_desc, sizeof(tmp_desc));

#ifdef IONIC_DEBUG_STATS
	stats->csum_none++;
#endif
}

static void ionic_tx_skb_frags(struct ionic_queue *q, struct sk_buff *skb,
			       struct ionic_desc_info *desc_info)
{
	struct ionic_txq_sg_desc *sg_desc = desc_info->txq_sg_desc;
	struct ionic_buf_info *buf_info = &desc_info->bufs[1];
	struct ionic_txq_sg_elem *elem = sg_desc->elems;
#ifdef IONIC_DEBUG_STATS
	struct ionic_tx_stats *stats = q_to_tx_stats(q);
#endif
	unsigned int i;

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++, buf_info++, elem++) {
		elem->addr = cpu_to_le64(buf_info->dma_addr);
		elem->len = cpu_to_le16(buf_info->len);
	}

#ifdef IONIC_DEBUG_STATS
	stats->frags += skb_shinfo(skb)->nr_frags;
#endif
}

static int ionic_tx(struct ionic_queue *q, struct sk_buff *skb)
{
	struct ionic_desc_info *desc_info = &q->info[q->head_idx];
	struct ionic_tx_stats *stats = q_to_tx_stats(q);

	if (unlikely(ionic_tx_map_skb(q, skb, desc_info)))
		return -EIO;

	/* set up the initial descriptor */
	if (skb->ip_summed == CHECKSUM_PARTIAL)
		ionic_tx_calc_csum(q, skb, desc_info);
	else
		ionic_tx_calc_no_csum(q, skb, desc_info);

	/* add frags */
	ionic_tx_skb_frags(q, skb, desc_info);

	skb_tx_timestamp(skb);
	stats->pkts++;
	stats->bytes += skb->len;

#ifdef IONIC_SUPPORTS_BQL
	if (!unlikely(q->features & IONIC_TXQ_F_HWSTAMP))
		netdev_tx_sent_queue(q_to_ndq(q), skb->len);
#endif
#ifdef HAVE_SKB_XMIT_MORE
	ionic_txq_post(q, !netdev_xmit_more(), ionic_tx_clean, skb);
#else
	ionic_txq_post(q, true, ionic_tx_clean, skb);
#endif

	return 0;
}

static int ionic_tx_descs_needed(struct ionic_queue *q, struct sk_buff *skb)
{
	struct ionic_tx_stats *stats = q_to_tx_stats(q);
	int ndescs;
	int err;

	/* Each desc is mss long max, so a descriptor for each gso_seg */
	if (skb_is_gso(skb))
		ndescs = skb_shinfo(skb)->gso_segs;
	else
		ndescs = 1;

	/* If non-TSO, just need 1 desc and nr_frags sg elems */
	if (skb_shinfo(skb)->nr_frags <= q->max_sg_elems)
		return ndescs;

	/* Too many frags, so linearize */
	err = skb_linearize(skb);
	if (err)
		return err;

	stats->linearize++;

	return ndescs;
}

static int ionic_maybe_stop_tx(struct ionic_queue *q, int ndescs)
{
	int stopped = 0;

	if (unlikely(!ionic_q_has_space(q, ndescs))) {
		netif_stop_subqueue(q->lif->netdev, q->index);
		q->stop++;
		stopped = 1;

		/* Might race with ionic_tx_clean, check again */
		smp_rmb();
		if (ionic_q_has_space(q, ndescs)) {
			netif_wake_subqueue(q->lif->netdev, q->index);
			stopped = 0;
		}
	}

	return stopped;
}

#if IS_ENABLED(CONFIG_PTP_1588_CLOCK)
static netdev_tx_t ionic_start_hwstamp_xmit(struct sk_buff *skb,
					    struct net_device *netdev)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic_queue *q = &lif->hwstamp_txq->q;
	int err, ndescs;

	/* Does not stop/start txq, because we post to a separate tx queue
	 * for timestamping, and if a packet can't be posted immediately to
	 * the timestamping queue, it is dropped.
	 */

	ndescs = ionic_tx_descs_needed(q, skb);
	if (unlikely(ndescs < 0))
		goto err_out_drop;

	if (unlikely(!ionic_q_has_space(q, ndescs)))
		goto err_out_drop;

	skb_shinfo(skb)->tx_flags |= SKBTX_HW_TSTAMP;
	if (skb_is_gso(skb))
		err = ionic_tx_tso(q, skb);
	else
		err = ionic_tx(q, skb);

	if (err)
		goto err_out_drop;

	return NETDEV_TX_OK;

err_out_drop:
	q->drop++;
	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
}
#endif

netdev_tx_t ionic_start_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	u16 queue_index = skb_get_queue_mapping(skb);
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic_queue *q;
	int ndescs;
	int err;

	if (unlikely(!test_bit(IONIC_LIF_F_UP, lif->state))) {
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

#if IS_ENABLED(CONFIG_PTP_1588_CLOCK)
	if (unlikely(skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP))
		if (lif->hwstamp_txq && lif->phc->ts_config_tx_mode)
			return ionic_start_hwstamp_xmit(skb, netdev);
#endif

	if (unlikely(queue_index >= lif->nxqs))
		queue_index = 0;
	q = &lif->txqcqs[queue_index]->q;

	ndescs = ionic_tx_descs_needed(q, skb);
	if (ndescs < 0)
		goto err_out_drop;

	if (unlikely(ionic_maybe_stop_tx(q, ndescs)))
		return NETDEV_TX_BUSY;

	if (skb_is_gso(skb))
		err = ionic_tx_tso(q, skb);
	else
		err = ionic_tx(q, skb);

	if (err)
		goto err_out_drop;

	/* Stop the queue if there aren't descriptors for the next packet.
	 * Since our SG lists per descriptor take care of most of the possible
	 * fragmentation, we don't need to have many descriptors available.
	 */
	ionic_maybe_stop_tx(q, 4);

	return NETDEV_TX_OK;

err_out_drop:
	q->stop++;
	q->drop++;
	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
}
