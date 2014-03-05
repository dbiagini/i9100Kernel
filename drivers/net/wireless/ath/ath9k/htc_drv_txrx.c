/*
 * Copyright (c) 2010-2011 Atheros Communications Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "htc.h"

/******/
/* TX */
/******/

static const int subtype_txq_to_hwq[] = {
	[WME_AC_BE] = ATH_TXQ_AC_BE,
	[WME_AC_BK] = ATH_TXQ_AC_BK,
	[WME_AC_VI] = ATH_TXQ_AC_VI,
	[WME_AC_VO] = ATH_TXQ_AC_VO,
};

#define ATH9K_HTC_INIT_TXQ(subtype) do {			\
		qi.tqi_subtype = subtype_txq_to_hwq[subtype];	\
		qi.tqi_aifs = ATH9K_TXQ_USEDEFAULT;		\
		qi.tqi_cwmin = ATH9K_TXQ_USEDEFAULT;		\
		qi.tqi_cwmax = ATH9K_TXQ_USEDEFAULT;		\
		qi.tqi_physCompBuf = 0;				\
		qi.tqi_qflags = TXQ_FLAG_TXEOLINT_ENABLE |	\
			TXQ_FLAG_TXDESCINT_ENABLE;		\
	} while (0)

int get_hw_qnum(u16 queue, int *hwq_map)
{
	switch (queue) {
	case 0:
		return hwq_map[WME_AC_VO];
	case 1:
		return hwq_map[WME_AC_VI];
	case 2:
		return hwq_map[WME_AC_BE];
	case 3:
		return hwq_map[WME_AC_BK];
	default:
		return hwq_map[WME_AC_BE];
	}
}

void ath9k_htc_check_stop_queues(struct ath9k_htc_priv *priv)
{
	spin_lock_bh(&priv->tx.tx_lock);
	priv->tx.queued_cnt++;
	if ((priv->tx.queued_cnt >= ATH9K_HTC_TX_THRESHOLD) &&
	    !(priv->tx.flags & ATH9K_HTC_OP_TX_QUEUES_STOP)) {
		priv->tx.flags |= ATH9K_HTC_OP_TX_QUEUES_STOP;
		ieee80211_stop_queues(priv->hw);
	}
	spin_unlock_bh(&priv->tx.tx_lock);
}

void ath9k_htc_check_wake_queues(struct ath9k_htc_priv *priv)
{
	spin_lock_bh(&priv->tx.tx_lock);
	if ((priv->tx.queued_cnt < ATH9K_HTC_TX_THRESHOLD) &&
	    (priv->tx.flags & ATH9K_HTC_OP_TX_QUEUES_STOP)) {
		priv->tx.flags &= ~ATH9K_HTC_OP_TX_QUEUES_STOP;
		ieee80211_wake_queues(priv->hw);
	}
	spin_unlock_bh(&priv->tx.tx_lock);
}

int ath9k_htc_tx_get_slot(struct ath9k_htc_priv *priv)
{
	int slot;

	spin_lock_bh(&priv->tx.tx_lock);
	slot = find_first_zero_bit(priv->tx.tx_slot, MAX_TX_BUF_NUM);
	if (slot >= MAX_TX_BUF_NUM) {
		spin_unlock_bh(&priv->tx.tx_lock);
		return -ENOBUFS;
	}
	__set_bit(slot, priv->tx.tx_slot);
	spin_unlock_bh(&priv->tx.tx_lock);

	return slot;
}

void ath9k_htc_tx_clear_slot(struct ath9k_htc_priv *priv, int slot)
{
	spin_lock_bh(&priv->tx.tx_lock);
	__clear_bit(slot, priv->tx.tx_slot);
	spin_unlock_bh(&priv->tx.tx_lock);
}

static inline enum htc_endpoint_id get_htc_epid(struct ath9k_htc_priv *priv,
						u16 qnum)
{
	enum htc_endpoint_id epid;

	switch (qnum) {
	case 0:
		TX_QSTAT_INC(WME_AC_VO);
		epid = priv->data_vo_ep;
		break;
	case 1:
		TX_QSTAT_INC(WME_AC_VI);
		epid = priv->data_vi_ep;
		break;
	case 2:
		TX_QSTAT_INC(WME_AC_BE);
		epid = priv->data_be_ep;
		break;
	case 3:
	default:
		TX_QSTAT_INC(WME_AC_BK);
		epid = priv->data_bk_ep;
		break;
	}

	return epid;
}

static inline struct sk_buff_head*
get_htc_epid_queue(struct ath9k_htc_priv *priv, u8 epid)
{
	struct ath_common *common = ath9k_hw_common(priv->ah);
	struct sk_buff_head *epid_queue = NULL;

	if (epid == priv->mgmt_ep)
		epid_queue = &priv->tx.mgmt_ep_queue;
	else if (epid == priv->cab_ep)
		epid_queue = &priv->tx.cab_ep_queue;
	else if (epid == priv->data_be_ep)
		epid_queue = &priv->tx.data_be_queue;
	else if (epid == priv->data_bk_ep)
		epid_queue = &priv->tx.data_bk_queue;
	else if (epid == priv->data_vi_ep)
		epid_queue = &priv->tx.data_vi_queue;
	else if (epid == priv->data_vo_ep)
		epid_queue = &priv->tx.data_vo_queue;
	else
		ath_err(common, "Invalid EPID: %d\n", epid);

	return epid_queue;
}

/*
 * Removes the driver header and returns the TX slot number
 */
static inline int strip_drv_header(struct ath9k_htc_priv *priv,
				   struct sk_buff *skb)
{
	struct ath_common *common = ath9k_hw_common(priv->ah);
	struct ath9k_htc_tx_ctl *tx_ctl;
	int slot;

	tx_ctl = HTC_SKB_CB(skb);

	if (tx_ctl->epid == priv->mgmt_ep) {
		struct tx_mgmt_hdr *tx_mhdr =
			(struct tx_mgmt_hdr *)skb->data;
		slot = tx_mhdr->cookie;
		skb_pull(skb, sizeof(struct tx_mgmt_hdr));
	} else if ((tx_ctl->epid == priv->data_bk_ep) ||
		   (tx_ctl->epid == priv->data_be_ep) ||
		   (tx_ctl->epid == priv->data_vi_ep) ||
		   (tx_ctl->epid == priv->data_vo_ep) ||
		   (tx_ctl->epid == priv->cab_ep)) {
		struct tx_frame_hdr *tx_fhdr =
			(struct tx_frame_hdr *)skb->data;
		slot = tx_fhdr->cookie;
		skb_pull(skb, sizeof(struct tx_frame_hdr));
	} else {
		ath_err(common, "Unsupported EPID: %d\n", tx_ctl->epid);
		slot = -EINVAL;
	}

	return slot;
}

int ath_htc_txq_update(struct ath9k_htc_priv *priv, int qnum,
		       struct ath9k_tx_queue_info *qinfo)
{
	struct ath_hw *ah = priv->ah;
	int error = 0;
	struct ath9k_tx_queue_info qi;

	ath9k_hw_get_txq_props(ah, qnum, &qi);

	qi.tqi_aifs = qinfo->tqi_aifs;
	qi.tqi_cwmin = qinfo->tqi_cwmin / 2; /* XXX */
	qi.tqi_cwmax = qinfo->tqi_cwmax;
	qi.tqi_burstTime = qinfo->tqi_burstTime;
	qi.tqi_readyTime = qinfo->tqi_readyTime;

	if (!ath9k_hw_set_txq_props(ah, qnum, &qi)) {
		ath_err(ath9k_hw_common(ah),
			"Unable to update hardware queue %u!\n", qnum);
		error = -EIO;
	} else {
		ath9k_hw_resettxqueue(ah, qnum);
	}

	return error;
}

static void ath9k_htc_tx_mgmt(struct ath9k_htc_priv *priv,
			      struct ath9k_htc_vif *avp,
			      struct sk_buff *skb,
			      u8 sta_idx, u8 vif_idx, u8 slot)
{
	struct ieee80211_tx_info *tx_info = IEEE80211_SKB_CB(skb);
	struct ieee80211_mgmt *mgmt;
	struct ieee80211_hdr *hdr;
	struct tx_mgmt_hdr mgmt_hdr;
	struct ath9k_htc_tx_ctl *tx_ctl;
	u8 *tx_fhdr;

	tx_ctl = HTC_SKB_CB(skb);
	hdr = (struct ieee80211_hdr *) skb->data;

	memset(tx_ctl, 0, sizeof(*tx_ctl));
	memset(&mgmt_hdr, 0, sizeof(struct tx_mgmt_hdr));

	/*
	 * Set the TSF adjust value for probe response
	 * frame also.
	 */
	if (avp && unlikely(ieee80211_is_probe_resp(hdr->frame_control))) {
		mgmt = (struct ieee80211_mgmt *)skb->data;
		mgmt->u.probe_resp.timestamp = avp->tsfadjust;
	}

	tx_ctl->type = ATH9K_HTC_MGMT;

	mgmt_hdr.node_idx = sta_idx;
	mgmt_hdr.vif_idx = vif_idx;
	mgmt_hdr.tidno = 0;
	mgmt_hdr.flags = 0;
	mgmt_hdr.cookie = slot;

	mgmt_hdr.key_type = ath9k_cmn_get_hw_crypto_keytype(skb);
	if (mgmt_hdr.key_type == ATH9K_KEY_TYPE_CLEAR)
		mgmt_hdr.keyix = (u8) ATH9K_TXKEYIX_INVALID;
	else
		mgmt_hdr.keyix = tx_info->control.hw_key->hw_key_idx;

	tx_fhdr = skb_push(skb, sizeof(mgmt_hdr));
	memcpy(tx_fhdr, (u8 *) &mgmt_hdr, sizeof(mgmt_hdr));
	tx_ctl->epid = priv->mgmt_ep;
}

static void ath9k_htc_tx_data(struct ath9k_htc_priv *priv,
			      struct ieee80211_vif *vif,
			      struct sk_buff *skb,
			      u8 sta_idx, u8 vif_idx, u8 slot,
			      bool is_cab)
{
	struct ieee80211_tx_info *tx_info = IEEE80211_SKB_CB(skb);
	struct ieee80211_hdr *hdr;
	struct ath9k_htc_tx_ctl *tx_ctl;
	struct tx_frame_hdr tx_hdr;
	u32 flags = 0;
	u8 *qc, *tx_fhdr;
	u16 qnum;

	tx_ctl = HTC_SKB_CB(skb);
	hdr = (struct ieee80211_hdr *) skb->data;

	memset(tx_ctl, 0, sizeof(*tx_ctl));
	memset(&tx_hdr, 0, sizeof(struct tx_frame_hdr));

	tx_hdr.node_idx = sta_idx;
	tx_hdr.vif_idx = vif_idx;
	tx_hdr.cookie = slot;

	/*
	 * This is a bit redundant but it helps to get
	 * the per-packet index quickly when draining the
	 * TX queue in the HIF layer. Otherwise we would
	 * have to parse the packet contents ...
	 */
	tx_ctl->sta_idx = sta_idx;

	if (tx_info->flags & IEEE80211_TX_CTL_AMPDU) {
		tx_ctl->type = ATH9K_HTC_AMPDU;
		tx_hdr.data_type = ATH9K_HTC_AMPDU;
	} else {
		tx_ctl->type = ATH9K_HTC_NORMAL;
		tx_hdr.data_type = ATH9K_HTC_NORMAL;
	}

	if (ieee80211_is_data_qos(hdr->frame_control)) {
		qc = ieee80211_get_qos_ctl(hdr);
		tx_hdr.tidno = qc[0] & IEEE80211_QOS_CTL_TID_MASK;
	}

	/* Check for RTS protection */
	if (priv->hw->wiphy->rts_threshold != (u32) -1)
		if (skb->len > priv->hw->wiphy->rts_threshold)
			flags |= ATH9K_HTC_TX_RTSCTS;

	/* CTS-to-self */
	if (!(flags & ATH9K_HTC_TX_RTSCTS) &&
	    (vif && vif->bss_conf.use_cts_prot))
		flags |= ATH9K_HTC_TX_CTSONLY;

	tx_hdr.flags = cpu_to_be32(flags);
	tx_hdr.key_type = ath9k_cmn_get_hw_crypto_keytype(skb);
	if (tx_hdr.key_type == ATH9K_KEY_TYPE_CLEAR)
		tx_hdr.keyix = (u8) ATH9K_TXKEYIX_INVALID;
	else
		tx_hdr.keyix = tx_info->control.hw_key->hw_key_idx;

	tx_fhdr = skb_push(skb, sizeof(tx_hdr));
	memcpy(tx_fhdr, (u8 *) &tx_hdr, sizeof(tx_hdr));

	if (is_cab) {
		CAB_STAT_INC;
		tx_ctl->epid = priv->cab_ep;
		return;
	}

	qnum = skb_get_queue_mapping(skb);
	tx_ctl->epid = get_htc_epid(priv, qnum);
}

int ath9k_htc_tx_start(struct ath9k_htc_priv *priv,
		       struct sk_buff *skb,
		       u8 slot, bool is_cab)
{
	struct ieee80211_hdr *hdr;
	struct ieee80211_tx_info *tx_info = IEEE80211_SKB_CB(skb);
	struct ieee80211_sta *sta = tx_info->control.sta;
	struct ieee80211_vif *vif = tx_info->control.vif;
	struct ath9k_htc_sta *ista;
	struct ath9k_htc_vif *avp = NULL;
	u8 sta_idx, vif_idx;

	hdr = (struct ieee80211_hdr *) skb->data;

	/*
	 * Find out on which interface this packet has to be
	 * sent out.
	 */
	if (vif) {
		avp = (struct ath9k_htc_vif *) vif->drv_priv;
		vif_idx = avp->index;
	} else {
		if (!priv->ah->is_monitoring) {
			ath_dbg(ath9k_hw_common(priv->ah), ATH_DBG_XMIT,
				"VIF is null, but no monitor interface !\n");
			return -EINVAL;
		}

		vif_idx = priv->mon_vif_idx;
	}

	/*
	 * Find out which station this packet is destined for.
	 */
	if (sta) {
		ista = (struct ath9k_htc_sta *) sta->drv_priv;
		sta_idx = ista->index;
	} else {
		sta_idx = priv->vif_sta_pos[vif_idx];
	}

	if (ieee80211_is_data(hdr->frame_control))
		ath9k_htc_tx_data(priv, vif, skb,
				  sta_idx, vif_idx, slot, is_cab);
	else
		ath9k_htc_tx_mgmt(priv, avp, skb,
				  sta_idx, vif_idx, slot);


	return htc_send(priv->htc, skb);
}

static inline bool __ath9k_htc_check_tx_aggr(struct ath9k_htc_priv *priv,
					     struct ath9k_htc_sta *ista, u8 tid)
{
	bool ret = false;

	spin_lock_bh(&priv->tx.tx_lock);
	if ((tid < ATH9K_HTC_MAX_TID) && (ista->tid_state[tid] == AGGR_STOP))
		ret = true;
	spin_unlock_bh(&priv->tx.tx_lock);

	return ret;
}

static void ath9k_htc_check_tx_aggr(struct ath9k_htc_priv *priv,
				    struct ieee80211_vif *vif,
				    struct sk_buff *skb)
{
	struct ieee80211_sta *sta;
	struct ieee80211_hdr *hdr;
	__le16 fc;

	hdr = (struct ieee80211_hdr *) skb->data;
	fc = hdr->frame_control;

	rcu_read_lock();

	sta = ieee80211_find_sta(vif, hdr->addr1);
	if (!sta) {
		rcu_read_unlock();
		return;
	}

	if (sta && conf_is_ht(&priv->hw->conf) &&
	    !(skb->protocol == cpu_to_be16(ETH_P_PAE))) {
		if (ieee80211_is_data_qos(fc)) {
			u8 *qc, tid;
			struct ath9k_htc_sta *ista;

			qc = ieee80211_get_qos_ctl(hdr);
			tid = qc[0] & 0xf;
			ista = (struct ath9k_htc_sta *)sta->drv_priv;
			if (__ath9k_htc_check_tx_aggr(priv, ista, tid)) {
				ieee80211_start_tx_ba_session(sta, tid, 0);
				spin_lock_bh(&priv->tx.tx_lock);
				ista->tid_state[tid] = AGGR_PROGRESS;
				spin_unlock_bh(&priv->tx.tx_lock);
			}
		}
	}

	rcu_read_unlock();
}

static void ath9k_htc_tx_process(struct ath9k_htc_priv *priv,
				 struct sk_buff *skb,
				 struct __wmi_event_txstatus *txs)
{
	struct ieee80211_vif *vif;
	struct ath9k_htc_tx_ctl *tx_ctl;
	struct ieee80211_tx_info *tx_info;
	struct ieee80211_tx_rate *rate;
	struct ieee80211_conf *cur_conf = &priv->hw->conf;
	bool txok;
	int slot;

	slot = strip_drv_header(priv, skb);
	if (slot < 0) {
		dev_kfree_skb_any(skb);
		return;
	}

	tx_ctl = HTC_SKB_CB(skb);
	txok = tx_ctl->txok;
	tx_info = IEEE80211_SKB_CB(skb);
	vif = tx_info->control.vif;
	rate = &tx_info->status.rates[0];

	memset(&tx_info->status, 0, sizeof(tx_info->status));

	/*
	 * URB submission failed for this frame, it never reached
	 * the target.
	 */
	if (!txok || !vif || !txs)
		goto send_mac80211;

	if (txs->ts_flags & ATH9K_HTC_TXSTAT_ACK)
		tx_info->flags |= IEEE80211_TX_STAT_ACK;

	if (txs->ts_flags & ATH9K_HTC_TXSTAT_FILT)
		tx_info->flags |= IEEE80211_TX_STAT_TX_FILTERED;

	if (txs->ts_flags & ATH9K_HTC_TXSTAT_RTC_CTS)
		rate->flags |= IEEE80211_TX_RC_USE_RTS_CTS;

	rate->count = 1;
	rate->idx = MS(txs->ts_rate, ATH9K_HTC_TXSTAT_RATE);

	if (txs->ts_flags & ATH9K_HTC_TXSTAT_MCS) {
		rate->flags |= IEEE80211_TX_RC_MCS;

		if (txs->ts_flags & ATH9K_HTC_TXSTAT_CW40)
			rate->flags |= IEEE80211_TX_RC_40_MHZ_WIDTH;
		if (txs->ts_flags & ATH9K_HTC_TXSTAT_SGI)
			rate->flags |= IEEE80211_TX_RC_SHORT_GI;
	} else {
		if (cur_conf->channel->band == IEEE80211_BAND_5GHZ)
			rate->idx += 4; /* No CCK rates */
	}

	ath9k_htc_check_tx_aggr(priv, vif, skb);

send_mac80211:
	spin_lock_bh(&priv->tx.tx_lock);
	if (WARN_ON(--priv->tx.queued_cnt < 0))
		priv->tx.queued_cnt = 0;
	spin_unlock_bh(&priv->tx.tx_lock);

	ath9k_htc_tx_clear_slot(priv, slot);

	/* Send status to mac80211 */
	ieee80211_tx_status(priv->hw, skb);
}

static inline void ath9k_htc_tx_drainq(struct ath9k_htc_priv *priv,
				       struct sk_buff_head *queue)
{
	struct sk_buff *skb;

	while ((skb = skb_dequeue(queue)) != NULL) {
		ath9k_htc_tx_process(priv, skb, NULL);
	}
}

void ath9k_htc_tx_drain(struct ath9k_htc_priv *priv)
{
	struct ath9k_htc_tx_event *event, *tmp;

	spin_lock_bh(&priv->tx.tx_lock);
	priv->tx.flags |= ATH9K_HTC_OP_TX_DRAIN;
	spin_unlock_bh(&priv->tx.tx_lock);

	/*
	 * Ensure that all pending TX frames are flushed,
	 * and that the TX completion/failed tasklets is killed.
	 */
	htc_stop(priv->htc);
	tasklet_kill(&priv->wmi->wmi_event_tasklet);
	tasklet_kill(&priv->tx_failed_tasklet);

	ath9k_htc_tx_drainq(priv, &priv->tx.mgmt_ep_queue);
	ath9k_htc_tx_drainq(priv, &priv->tx.cab_ep_queue);
	ath9k_htc_tx_drainq(priv, &priv->tx.data_be_queue);
	ath9k_htc_tx_drainq(priv, &priv->tx.data_bk_queue);
	ath9k_htc_tx_drainq(priv, &priv->tx.data_vi_queue);
	ath9k_htc_tx_drainq(priv, &priv->tx.data_vo_queue);
	ath9k_htc_tx_drainq(priv, &priv->tx.tx_failed);

	/*
	 * The TX cleanup timer has already been killed.
	 */
	spin_lock_bh(&priv->wmi->event_lock);
	list_for_each_entry_safe(event, tmp, &priv->wmi->pending_tx_events, list) {
		list_del(&event->list);
		kfree(event);
	}
	spin_unlock_bh(&priv->wmi->event_lock);

	spin_lock_bh(&priv->tx.tx_lock);
	priv->tx.flags &= ~ATH9K_HTC_OP_TX_DRAIN;
	spin_unlock_bh(&priv->tx.tx_lock);
}

void ath9k_tx_failed_tasklet(unsigned long data)
{
	struct ath9k_htc_priv *priv = (struct ath9k_htc_priv *)data;

	spin_lock_bh(&priv->tx.tx_lock);
	if (priv->tx.flags & ATH9K_HTC_OP_TX_DRAIN) {
		spin_unlock_bh(&priv->tx.tx_lock);
		return;
	}
	spin_unlock_bh(&priv->tx.tx_lock);

	ath9k_htc_tx_drainq(priv, &priv->tx.tx_failed);
}

static inline bool check_cookie(struct ath9k_htc_priv *priv,
				struct sk_buff *skb,
				u8 cookie, u8 epid)
{
	u8 fcookie = 0;

	if (epid == priv->mgmt_ep) {
		struct tx_mgmt_hdr *hdr;
		hdr = (struct tx_mgmt_hdr *) skb->data;
		fcookie = hdr->cookie;
	} else if ((epid == priv->data_bk_ep) ||
		   (epid == priv->data_be_ep) ||
		   (epid == priv->data_vi_ep) ||
		   (epid == priv->data_vo_ep) ||
		   (epid == priv->cab_ep)) {
		struct tx_frame_hdr *hdr;
		hdr = (struct tx_frame_hdr *) skb->data;
		fcookie = hdr->cookie;
	}

	if (fcookie == cookie)
		return true;

	return false;
}

static struct sk_buff* ath9k_htc_tx_get_packet(struct ath9k_htc_priv *priv,
					       struct __wmi_event_txstatus *txs)
{
	struct ath_common *common = ath9k_hw_common(priv->ah);
	struct sk_buff_head *epid_queue;
	struct sk_buff *skb, *tmp;
	unsigned long flags;
	u8 epid = MS(txs->ts_rate, ATH9K_HTC_TXSTAT_EPID);

	epid_queue = get_htc_epid_queue(priv, epid);
	if (!epid_queue)
		return NULL;

	spin_lock_irqsave(&epid_queue->lock, flags);
	skb_queue_walk_safe(epid_queue, skb, tmp) {
		if (check_cookie(priv, skb, txs->cookie, epid)) {
			__skb_unlink(skb, epid_queue);
			spin_unlock_irqrestore(&epid_queue->lock, flags);
			return skb;
		}
	}
	spin_unlock_irqrestore(&epid_queue->lock, flags);

	ath_dbg(common, ATH_DBG_XMIT,
		"No matching packet for cookie: %d, epid: %d\n",
		txs->cookie, epid);

	return NULL;
}

void ath9k_htc_txstatus(struct ath9k_htc_priv *priv, void *wmi_event)
{
	struct wmi_event_txstatus *txs = (struct wmi_event_txstatus *)wmi_event;
	struct __wmi_event_txstatus *__txs;
	struct sk_buff *skb;
	struct ath9k_htc_tx_event *tx_pend;
	int i;

	for (i = 0; i < txs->cnt; i++) {
		WARN_ON(txs->cnt > HTC_MAX_TX_STATUS);

		__txs = &txs->txstatus[i];

		skb = ath9k_htc_tx_get_packet(priv, __txs);
		if (!skb) {
			/*
			 * Store this event, so that the TX cleanup
			 * routine can check later for the needed packet.
			 */
			tx_pend = kzalloc(sizeof(struct ath9k_htc_tx_event),
					  GFP_ATOMIC);
			if (!tx_pend)
				continue;

			memcpy(&tx_pend->txs, __txs,
			       sizeof(struct __wmi_event_txstatus));

			spin_lock(&priv->wmi->event_lock);
			list_add_tail(&tx_pend->list,
				      &priv->wmi->pending_tx_events);
			spin_unlock(&priv->wmi->event_lock);

			continue;
		}

		ath9k_htc_tx_process(priv, skb, __txs);
	}

	/* Wake TX queues if needed */
	ath9k_htc_check_wake_queues(priv);
}

void ath9k_htc_txep(void *drv_priv, struct sk_buff *skb,
		    enum htc_endpoint_id ep_id, bool txok)
{
	struct ath9k_htc_priv *priv = (struct ath9k_htc_priv *) drv_priv;
	struct ath9k_htc_tx_ctl *tx_ctl;
	struct sk_buff_head *epid_queue;

	tx_ctl = HTC_SKB_CB(skb);
	tx_ctl->txok = txok;
	tx_ctl->timestamp = jiffies;

	if (!txok) {
		skb_queue_tail(&priv->tx.tx_failed, skb);
		tasklet_schedule(&priv->tx_failed_tasklet);
		return;
	}

	epid_queue = get_htc_epid_queue(priv, ep_id);
	if (!epid_queue) {
		dev_kfree_skb_any(skb);
		return;
	}

	skb_queue_tail(epid_queue, skb);
}

static inline bool check_packet(struct ath9k_htc_priv *priv, struct sk_buff *skb)
{
	struct ath_common *common = ath9k_hw_common(priv->ah);
	struct ath9k_htc_tx_ctl *tx_ctl;

	tx_ctl = HTC_SKB_CB(skb);

	if (time_after(jiffies,
		       tx_ctl->timestamp +
		       msecs_to_jiffies(ATH9K_HTC_TX_TIMEOUT_INTERVAL))) {
		ath_dbg(common, ATH_DBG_XMIT,
			"Dropping a packet due to TX timeout\n");
		return true;
	}

	return false;
}

static void ath9k_htc_tx_cleanup_queue(struct ath9k_htc_priv *priv,
				       struct sk_buff_head *epid_queue)
{
	bool process = false;
	unsigned long flags;
	struct sk_buff *skb, *tmp;
	struct sk_buff_head queue;

	skb_queue_head_init(&queue);

	spin_lock_irqsave(&epid_queue->lock, flags);
	skb_queue_walk_safe(epid_queue, skb, tmp) {
		if (check_packet(priv, skb)) {
			__skb_unlink(skb, epid_queue);
			__skb_queue_tail(&queue, skb);
			process = true;
		}
	}
	spin_unlock_irqrestore(&epid_queue->lock, flags);

	if (process) {
		skb_queue_walk_safe(&queue, skb, tmp) {
			__skb_unlink(skb, &queue);
			ath9k_htc_tx_process(priv, skb, NULL);
		}
	}
}

void ath9k_htc_tx_cleanup_timer(unsigned long data)
{
	struct ath9k_htc_priv *priv = (struct ath9k_htc_priv *) data;
	struct ath_common *common = ath9k_hw_common(priv->ah);
	struct ath9k_htc_tx_event *event, *tmp;
	struct sk_buff *skb;

	spin_lock(&priv->wmi->event_lock);
	list_for_each_entry_safe(event, tmp, &priv->wmi->pending_tx_events, list) {

		skb = ath9k_htc_tx_get_packet(priv, &event->txs);
		if (skb) {
			ath_dbg(common, ATH_DBG_XMIT,
				"Found packet for cookie: %d, epid: %d\n",
				event->txs.cookie,
				MS(event->txs.ts_rate, ATH9K_HTC_TXSTAT_EPID));

			ath9k_htc_tx_process(priv, skb, &event->txs);
			list_del(&event->list);
			kfree(event);
			continue;
		}

		if (++event->count >= ATH9K_HTC_TX_TIMEOUT_COUNT) {
			list_del(&event->list);
			kfree(event);
		}
	}
	spin_unlock(&priv->wmi->event_lock);

	/*
	 * Check if status-pending packets have to be cleaned up.
	 */
	ath9k_htc_tx_cleanup_queue(priv, &priv->tx.mgmt_ep_queue);
	ath9k_htc_tx_cleanup_queue(priv, &priv->tx.cab_ep_queue);
	ath9k_htc_tx_cleanup_queue(priv, &priv->tx.data_be_queue);
	ath9k_htc_tx_cleanup_queue(priv, &priv->tx.data_bk_queue);
	ath9k_htc_tx_cleanup_queue(priv, &priv->tx.data_vi_queue);
	ath9k_htc_tx_cleanup_queue(priv, &priv->tx.data_vo_queue);

	/* Wake TX queues if needed */
	ath9k_htc_check_wake_queues(priv);

	mod_timer(&priv->tx.cleanup_timer,
		  jiffies + msecs_to_jiffies(ATH9K_HTC_TX_CLEANUP_INTERVAL));
}

int ath9k_tx_init(struct ath9k_htc_priv *priv)
{
	skb_queue_head_init(&priv->tx.mgmt_ep_queue);
	skb_queue_head_init(&priv->tx.cab_ep_queue);
	skb_queue_head_init(&priv->tx.data_be_queue);
	skb_queue_head_init(&priv->tx.data_bk_queue);
	skb_queue_head_init(&priv->tx.data_vi_queue);
	skb_queue_head_init(&priv->tx.data_vo_queue);
	skb_queue_head_init(&priv->tx.tx_failed);
	return 0;
}

void ath9k_tx_cleanup(struct ath9k_htc_priv *priv)
{

}

bool ath9k_htc_txq_setup(struct ath9k_htc_priv *priv, int subtype)
{
	struct ath_hw *ah = priv->ah;
	struct ath_common *common = ath9k_hw_common(ah);
	struct ath9k_tx_queue_info qi;
	int qnum;

	memset(&qi, 0, sizeof(qi));
	ATH9K_HTC_INIT_TXQ(subtype);

	qnum = ath9k_hw_setuptxqueue(priv->ah, ATH9K_TX_QUEUE_DATA, &qi);
	if (qnum == -1)
		return false;

	if (qnum >= ARRAY_SIZE(priv->hwq_map)) {
		ath_err(common, "qnum %u out of range, max %zu!\n",
			qnum, ARRAY_SIZE(priv->hwq_map));
		ath9k_hw_releasetxqueue(ah, qnum);
		return false;
	}

	priv->hwq_map[subtype] = qnum;
	return true;
}

int ath9k_htc_cabq_setup(struct ath9k_htc_priv *priv)
{
	struct ath9k_tx_queue_info qi;

	memset(&qi, 0, sizeof(qi));
	ATH9K_HTC_INIT_TXQ(0);

	return ath9k_hw_setuptxqueue(priv->ah, ATH9K_TX_QUEUE_CAB, &qi);
}

/******/
/* RX */
/******/

/*
 * Calculate the RX filter to be set in the HW.
 */
u32 ath9k_htc_calcrxfilter(struct ath9k_htc_priv *priv)
{
#define	RX_FILTER_PRESERVE (ATH9K_RX_FILTER_PHYERR | ATH9K_RX_FILTER_PHYRADAR)

	struct ath_hw *ah = priv->ah;
	u32 rfilt;

	rfilt = (ath9k_hw_getrxfilter(ah) & RX_FILTER_PRESERVE)
		| ATH9K_RX_FILTER_UCAST | ATH9K_RX_FILTER_BCAST
		| ATH9K_RX_FILTER_MCAST;

	if (priv->rxfilter & FIF_PROBE_REQ)
		rfilt |= ATH9K_RX_FILTER_PROBEREQ;

	/*
	 * Set promiscuous mode when FIF_PROMISC_IN_BSS is enabled for station
	 * mode interface or when in monitor mode. AP mode does not need this
	 * since it receives all in-BSS frames anyway.
	 */
	if (((ah->opmode != NL80211_IFTYPE_AP) &&
	     (priv->rxfilter & FIF_PROMISC_IN_BSS)) ||
	    ah->is_monitoring)
		rfilt |= ATH9K_RX_FILTER_PROM;

	if (priv->rxfilter & FIF_CONTROL)
		rfilt |= ATH9K_RX_FILTER_CONTROL;

	if ((ah->opmode == NL80211_IFTYPE_STATION) &&
	    (priv->nvifs <= 1) &&
	    !(priv->rxfilter & FIF_BCN_PRBRESP_PROMISC))
		rfilt |= ATH9K_RX_FILTER_MYBEACON;
	else
		rfilt |= ATH9K_RX_FILTER_BEACON;

	if (conf_is_ht(&priv->hw->conf)) {
		rfilt |= ATH9K_RX_FILTER_COMP_BAR;
		rfilt |= ATH9K_RX_FILTER_UNCOMP_BA_BAR;
	}

	if (priv->rxfilter & FIF_PSPOLL)
		rfilt |= ATH9K_RX_FILTER_PSPOLL;

	if (priv->nvifs > 1)
		rfilt |= ATH9K_RX_FILTER_MCAST_BCAST_ALL;

	return rfilt;

#undef RX_FILTER_PRESERVE
}

/*
 * Recv initialization for opmode change.
 */
static void ath9k_htc_opmode_init(struct ath9k_htc_priv *priv)
{
	struct ath_hw *ah = priv->ah;
	u32 rfilt, mfilt[2];

	/* configure rx filter */
	rfilt = ath9k_htc_calcrxfilter(priv);
	ath9k_hw_setrxfilter(ah, rfilt);

	/* calculate and install multicast filter */
	mfilt[0] = mfilt[1] = ~0;
	ath9k_hw_setmcastfilter(ah, mfilt[0], mfilt[1]);
}

void ath9k_host_rx_init(struct ath9k_htc_priv *priv)
{
	ath9k_hw_rxena(priv->ah);
	ath9k_htc_opmode_init(priv);
	ath9k_hw_startpcureceive(priv->ah, (priv->op_flags & OP_SCANNING));
	priv->rx.last_rssi = ATH_RSSI_DUMMY_MARKER;
}

static void ath9k_process_rate(struct ieee80211_hw *hw,
			       struct ieee80211_rx_status *rxs,
			       u8 rx_rate, u8 rs_flags)
{
	struct ieee80211_supported_band *sband;
	enum ieee80211_band band;
	unsigned int i = 0;

	if (rx_rate & 0x80) {
		/* HT rate */
		rxs->flag |= RX_FLAG_HT;
		if (rs_flags & ATH9K_RX_2040)
			rxs->flag |= RX_FLAG_40MHZ;
		if (rs_flags & ATH9K_RX_GI)
			rxs->flag |= RX_FLAG_SHORT_GI;
		rxs->rate_idx = rx_rate & 0x7f;
		return;
	}

	band = hw->conf.channel->band;
	sband = hw->wiphy->bands[band];

	for (i = 0; i < sband->n_bitrates; i++) {
		if (sband->bitrates[i].hw_value == rx_rate) {
			rxs->rate_idx = i;
			return;
		}
		if (sband->bitrates[i].hw_value_short == rx_rate) {
			rxs->rate_idx = i;
			rxs->flag |= RX_FLAG_SHORTPRE;
			return;
		}
	}

}

static bool ath9k_rx_prepare(struct ath9k_htc_priv *priv,
			     struct ath9k_htc_rxbuf *rxbuf,
			     struct ieee80211_rx_status *rx_status)

{
	struct ieee80211_hdr *hdr;
	struct ieee80211_hw *hw = priv->hw;
	struct sk_buff *skb = rxbuf->skb;
	struct ath_common *common = ath9k_hw_common(priv->ah);
	struct ath_htc_rx_status *rxstatus;
	int hdrlen, padpos, padsize;
	int last_rssi = ATH_RSSI_DUMMY_MARKER;
	__le16 fc;

	if (skb->len < HTC_RX_FRAME_HEADER_SIZE) {
		ath_err(common, "Corrupted RX frame, dropping (len: %d)\n",
			skb->len);
		goto rx_next;
	}

	rxstatus = (struct ath_htc_rx_status *)skb->data;

	if (be16_to_cpu(rxstatus->rs_datalen) -
	    (skb->len - HTC_RX_FRAME_HEADER_SIZE) != 0) {
		ath_err(common,
			"Corrupted RX data len, dropping (dlen: %d, skblen: %d)\n",
			rxstatus->rs_datalen, skb->len);
		goto rx_next;
	}

	ath9k_htc_err_stat_rx(priv, rxstatus);

	/* Get the RX status information */
	memcpy(&rxbuf->rxstatus, rxstatus, HTC_RX_FRAME_HEADER_SIZE);
	skb_pull(skb, HTC_RX_FRAME_HEADER_SIZE);

	hdr = (struct ieee80211_hdr *)skb->data;
	fc = hdr->frame_control;
	hdrlen = ieee80211_get_hdrlen_from_skb(skb);

	padpos = ath9k_cmn_padpos(fc);

	padsize = padpos & 3;
	if (padsize && skb->len >= padpos+padsize+FCS_LEN) {
		memmove(skb->data + padsize, skb->data, padpos);
		skb_pull(skb, padsize);
	}

	memset(rx_status, 0, sizeof(struct ieee80211_rx_status));

	if (rxbuf->rxstatus.rs_status != 0) {
		if (rxbuf->rxstatus.rs_status & ATH9K_RXERR_CRC)
			rx_status->flag |= RX_FLAG_FAILED_FCS_CRC;
		if (rxbuf->rxstatus.rs_status & ATH9K_RXERR_PHY)
			goto rx_next;

		if (rxbuf->rxstatus.rs_status & ATH9K_RXERR_DECRYPT) {
			/* FIXME */
		} else if (rxbuf->rxstatus.rs_status & ATH9K_RXERR_MIC) {
			if (ieee80211_is_ctl(fc))
				/*
				 * Sometimes, we get invalid
				 * MIC failures on valid control frames.
				 * Remove these mic errors.
				 */
				rxbuf->rxstatus.rs_status &= ~ATH9K_RXERR_MIC;
			else
				rx_status->flag |= RX_FLAG_MMIC_ERROR;
		}

		/*
		 * Reject error frames with the exception of
		 * decryption and MIC failures. For monitor mode,
		 * we also ignore the CRC error.
		 */
		if (priv->ah->opmode == NL80211_IFTYPE_MONITOR) {
			if (rxbuf->rxstatus.rs_status &
			    ~(ATH9K_RXERR_DECRYPT | ATH9K_RXERR_MIC |
			      ATH9K_RXERR_CRC))
				goto rx_next;
		} else {
			if (rxbuf->rxstatus.rs_status &
			    ~(ATH9K_RXERR_DECRYPT | ATH9K_RXERR_MIC)) {
				goto rx_next;
			}
		}
	}

	if (!(rxbuf->rxstatus.rs_status & ATH9K_RXERR_DECRYPT)) {
		u8 keyix;
		keyix = rxbuf->rxstatus.rs_keyix;
		if (keyix != ATH9K_RXKEYIX_INVALID) {
			rx_status->flag |= RX_FLAG_DECRYPTED;
		} else if (ieee80211_has_protected(fc) &&
			   skb->len >= hdrlen + 4) {
			keyix = skb->data[hdrlen + 3] >> 6;
			if (test_bit(keyix, common->keymap))
				rx_status->flag |= RX_FLAG_DECRYPTED;
		}
	}

	ath9k_process_rate(hw, rx_status, rxbuf->rxstatus.rs_rate,
			   rxbuf->rxstatus.rs_flags);

	if (rxbuf->rxstatus.rs_rssi != ATH9K_RSSI_BAD &&
	    !rxbuf->rxstatus.rs_moreaggr)
		ATH_RSSI_LPF(priv->rx.last_rssi,
			     rxbuf->rxstatus.rs_rssi);

	last_rssi = priv->rx.last_rssi;

	if (likely(last_rssi != ATH_RSSI_DUMMY_MARKER))
		rxbuf->rxstatus.rs_rssi = ATH_EP_RND(last_rssi,
						     ATH_RSSI_EP_MULTIPLIER);

	if (rxbuf->rxstatus.rs_rssi < 0)
		rxbuf->rxstatus.rs_rssi = 0;

	if (ieee80211_is_beacon(fc))
		priv->ah->stats.avgbrssi = rxbuf->rxstatus.rs_rssi;

	rx_status->mactime = be64_to_cpu(rxbuf->rxstatus.rs_tstamp);
	rx_status->band = hw->conf.channel->band;
	rx_status->freq = hw->conf.channel->center_freq;
	rx_status->signal =  rxbuf->rxstatus.rs_rssi + ATH_DEFAULT_NOISE_FLOOR;
	rx_status->antenna = rxbuf->rxstatus.rs_antenna;
	rx_status->flag |= RX_FLAG_MACTIME_MPDU;

	return true;

rx_next:
	return false;
}

/*
 * FIXME: Handle FLUSH later on.
 */
void ath9k_rx_tasklet(unsigned long data)
{
	struct ath9k_htc_priv *priv = (struct ath9k_htc_priv *)data;
	struct ath9k_htc_rxbuf *rxbuf = NULL, *tmp_buf = NULL;
	struct ieee80211_rx_status rx_status;
	struct sk_buff *skb;
	unsigned long flags;
	struct ieee80211_hdr *hdr;

	do {
		spin_lock_irqsave(&priv->rx.rxbuflock, flags);
		list_for_each_entry(tmp_buf, &priv->rx.rxbuf, list) {
			if (tmp_buf->in_process) {
				rxbuf = tmp_buf;
				break;
			}
		}

		if (rxbuf == NULL) {
			spin_unlock_irqrestore(&priv->rx.rxbuflock, flags);
			break;
		}

		if (!rxbuf->skb)
			goto requeue;

		if (!ath9k_rx_prepare(priv, rxbuf, &rx_status)) {
			dev_kfree_skb_any(rxbuf->skb);
			goto requeue;
		}

		memcpy(IEEE80211_SKB_RXCB(rxbuf->skb), &rx_status,
		       sizeof(struct ieee80211_rx_status));
		skb = rxbuf->skb;
		hdr = (struct ieee80211_hdr *) skb->data;

		if (ieee80211_is_beacon(hdr->frame_control) && priv->ps_enabled)
				ieee80211_queue_work(priv->hw, &priv->ps_work);

		spin_unlock_irqrestore(&priv->rx.rxbuflock, flags);

		ieee80211_rx(priv->hw, skb);

		spin_lock_irqsave(&priv->rx.rxbuflock, flags);
requeue:
		rxbuf->in_process = false;
		rxbuf->skb = NULL;
		list_move_tail(&rxbuf->list, &priv->rx.rxbuf);
		rxbuf = NULL;
		spin_unlock_irqrestore(&priv->rx.rxbuflock, flags);
	} while (1);

}

void ath9k_htc_rxep(void *drv_priv, struct sk_buff *skb,
		    enum htc_endpoint_id ep_id)
{
	struct ath9k_htc_priv *priv = (struct ath9k_htc_priv *)drv_priv;
	struct ath_hw *ah = priv->ah;
	struct ath_common *common = ath9k_hw_common(ah);
	struct ath9k_htc_rxbuf *rxbuf = NULL, *tmp_buf = NULL;

	spin_lock(&priv->rx.rxbuflock);
	list_for_each_entry(tmp_buf, &priv->rx.rxbuf, list) {
		if (!tmp_buf->in_process) {
			rxbuf = tmp_buf;
			break;
		}
	}
	spin_unlock(&priv->rx.rxbuflock);

	if (rxbuf == NULL) {
		ath_dbg(common, ATH_DBG_ANY,
			"No free RX buffer\n");
		goto err;
	}

	spin_lock(&priv->rx.rxbuflock);
	rxbuf->skb = skb;
	rxbuf->in_process = true;
	spin_unlock(&priv->rx.rxbuflock);

	tasklet_schedule(&priv->rx_tasklet);
	return;
err:
	dev_kfree_skb_any(skb);
}

/* FIXME: Locking for cleanup/init */

void ath9k_rx_cleanup(struct ath9k_htc_priv *priv)
{
	struct ath9k_htc_rxbuf *rxbuf, *tbuf;

	list_for_each_entry_safe(rxbuf, tbuf, &priv->rx.rxbuf, list) {
		list_del(&rxbuf->list);
		if (rxbuf->skb)
			dev_kfree_skb_any(rxbuf->skb);
		kfree(rxbuf);
	}
}

int ath9k_rx_init(struct ath9k_htc_priv *priv)
{
	struct ath_hw *ah = priv->ah;
	struct ath_common *common = ath9k_hw_common(ah);
	struct ath9k_htc_rxbuf *rxbuf;
	int i = 0;

	INIT_LIST_HEAD(&priv->rx.rxbuf);
	spin_lock_init(&priv->rx.rxbuflock);

	for (i = 0; i < ATH9K_HTC_RXBUF; i++) {
		rxbuf = kzalloc(sizeof(struct ath9k_htc_rxbuf), GFP_KERNEL);
		if (rxbuf == NULL) {
			ath_err(common, "Unable to allocate RX buffers\n");
			goto err;
		}
		list_add_tail(&rxbuf->list, &priv->rx.rxbuf);
	}

	return 0;

err:
	ath9k_rx_cleanup(priv);
	return -ENOMEM;
}
