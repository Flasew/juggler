/*
 *	IPV4 GSO/GRO offload support
 *	Linux INET implementation
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 *
 *	TCPv4 GSO/GRO support
 */

#include <linux/skbuff.h>
#include <net/tcp.h>
#include <net/protocol.h>

static void tcp_gso_tstamp(struct sk_buff *skb, unsigned int ts_seq,
			   unsigned int seq, unsigned int mss)
{
	while (skb) {
		if (before(ts_seq, seq + mss)) {
			skb_shinfo(skb)->tx_flags |= SKBTX_SW_TSTAMP;
			skb_shinfo(skb)->tskey = ts_seq;
			return;
		}

		skb = skb->next;
		seq += mss;
	}
}

static struct sk_buff *tcp4_gso_segment(struct sk_buff *skb,
					netdev_features_t features)
{
	if (!(skb_shinfo(skb)->gso_type & SKB_GSO_TCPV4))
		return ERR_PTR(-EINVAL);

	if (!pskb_may_pull(skb, sizeof(struct tcphdr)))
		return ERR_PTR(-EINVAL);

	if (unlikely(skb->ip_summed != CHECKSUM_PARTIAL)) {
		const struct iphdr *iph = ip_hdr(skb);
		struct tcphdr *th = tcp_hdr(skb);

		/* Set up checksum pseudo header, usually expect stack to
		 * have done this already.
		 */

		th->check = 0;
		skb->ip_summed = CHECKSUM_PARTIAL;
		__tcp_v4_send_check(skb, iph->saddr, iph->daddr);
	}

	return tcp_gso_segment(skb, features);
}

struct sk_buff *tcp_gso_segment(struct sk_buff *skb,
				netdev_features_t features)
{
	struct sk_buff *segs = ERR_PTR(-EINVAL);
	unsigned int sum_truesize = 0;
	struct tcphdr *th;
	unsigned int thlen;
	unsigned int seq;
	__be32 delta;
	unsigned int oldlen;
	unsigned int mss;
	struct sk_buff *gso_skb = skb;
	__sum16 newcheck;
	bool ooo_okay, copy_destructor;

	th = tcp_hdr(skb);
	thlen = th->doff * 4;
	if (thlen < sizeof(*th))
		goto out;

	if (!pskb_may_pull(skb, thlen))
		goto out;

	oldlen = (u16)~skb->len;
	__skb_pull(skb, thlen);

	mss = skb_shinfo(skb)->gso_size;
	if (unlikely(skb->len <= mss))
		goto out;

	if (skb_gso_ok(skb, features | NETIF_F_GSO_ROBUST)) {
		/* Packet is from an untrusted source, reset gso_segs. */

		skb_shinfo(skb)->gso_segs = DIV_ROUND_UP(skb->len, mss);

		segs = NULL;
		goto out;
	}

	copy_destructor = gso_skb->destructor == tcp_wfree;
	ooo_okay = gso_skb->ooo_okay;
	/* All segments but the first should have ooo_okay cleared */
	skb->ooo_okay = 0;

	segs = skb_segment(skb, features);
	if (IS_ERR(segs))
		goto out;

	/* Only first segment might have ooo_okay set */
	segs->ooo_okay = ooo_okay;

	/* GSO partial and frag_list segmentation only requires splitting
	 * the frame into an MSS multiple and possibly a remainder, both
	 * cases return a GSO skb. So update the mss now.
	 */
	if (skb_is_gso(segs))
		mss *= skb_shinfo(segs)->gso_segs;

	delta = htonl(oldlen + (thlen + mss));

	skb = segs;
	th = tcp_hdr(skb);
	seq = ntohl(th->seq);

	if (unlikely(skb_shinfo(gso_skb)->tx_flags & SKBTX_SW_TSTAMP))
		tcp_gso_tstamp(segs, skb_shinfo(gso_skb)->tskey, seq, mss);

	newcheck = ~csum_fold((__force __wsum)((__force u32)th->check +
					       (__force u32)delta));

	while (skb->next) {
		th->fin = th->psh = 0;
		th->check = newcheck;

		if (skb->ip_summed == CHECKSUM_PARTIAL)
			gso_reset_checksum(skb, ~th->check);
		else
			th->check = gso_make_checksum(skb, ~th->check);

		seq += mss;
		if (copy_destructor) {
			skb->destructor = gso_skb->destructor;
			skb->sk = gso_skb->sk;
			sum_truesize += skb->truesize;
		}
		skb = skb->next;
		th = tcp_hdr(skb);

		th->seq = htonl(seq);
		th->cwr = 0;
	}

	/* Following permits TCP Small Queues to work well with GSO :
	 * The callback to TCP stack will be called at the time last frag
	 * is freed at TX completion, and not right now when gso_skb
	 * is freed by GSO engine
	 */
	if (copy_destructor) {
		int delta;

		swap(gso_skb->sk, skb->sk);
		swap(gso_skb->destructor, skb->destructor);
		sum_truesize += skb->truesize;
		delta = sum_truesize - gso_skb->truesize;
		/* In some pathological cases, delta can be negative.
		 * We need to either use refcount_add() or refcount_sub_and_test()
		 */
		if (likely(delta >= 0))
			refcount_add(delta, &skb->sk->sk_wmem_alloc);
		else
			WARN_ON_ONCE(refcount_sub_and_test(-delta, &skb->sk->sk_wmem_alloc));
	}

	delta = htonl(oldlen + (skb_tail_pointer(skb) -
				skb_transport_header(skb)) +
		      skb->data_len);
	th->check = ~csum_fold((__force __wsum)((__force u32)th->check +
				(__force u32)delta));
	if (skb->ip_summed == CHECKSUM_PARTIAL)
		gso_reset_checksum(skb, ~th->check);
	else
		th->check = gso_make_checksum(skb, ~th->check);
out:
	return segs;
}

struct sk_buff *tcp_gro_receive(struct list_head *head, struct sk_buff *skb)
{
// 	struct sk_buff *pp = NULL;
// 	struct sk_buff *p;
// 	struct tcphdr *th;
// 	struct tcphdr *th2;
// 	unsigned int len;
// 	unsigned int thlen;
// 	__be32 flags;
// 	unsigned int mss = 1;
// 	unsigned int hlen;
// 	unsigned int off;
// 	int flush = 1;
// 	int i;

// 	off = skb_gro_offset(skb);
// 	hlen = off + sizeof(*th);
// 	th = skb_gro_header_fast(skb, off);
// 	if (skb_gro_header_hard(skb, hlen)) {
// 		th = skb_gro_header_slow(skb, hlen, off);
// 		if (unlikely(!th))
// 			goto out;
// 	}

// 	thlen = th->doff * 4;
// 	if (thlen < sizeof(*th))
// 		goto out;

// 	hlen = off + thlen;
// 	if (skb_gro_header_hard(skb, hlen)) {
// 		th = skb_gro_header_slow(skb, hlen, off);
// 		if (unlikely(!th))
// 			goto out;
// 	}

// 	skb_gro_pull(skb, thlen);

// 	len = skb_gro_len(skb);
// 	flags = tcp_flag_word(th);

// 	list_for_each_entry(p, head, list) {
// 		if (!NAPI_GRO_CB(p)->same_flow)
// 			continue;

// 		th2 = tcp_hdr(p);

// 		if (*(u32 *)&th->source ^ *(u32 *)&th2->source) {
// 			NAPI_GRO_CB(p)->same_flow = 0;
// 			continue;
// 		}

// 		goto found;
// 	}
// 	p = NULL;
// 	goto out_check_final;

// found:
// 	/* Include the IP ID check below from the inner most IP hdr */
// 	flush = NAPI_GRO_CB(p)->flush;
// 	flush |= (__force int)(flags & TCP_FLAG_CWR);
// 	flush |= (__force int)((flags ^ tcp_flag_word(th2)) &
// 		  ~(TCP_FLAG_CWR | TCP_FLAG_FIN | TCP_FLAG_PSH));
// 	flush |= (__force int)(th->ack_seq ^ th2->ack_seq);
// 	for (i = sizeof(*th); i < thlen; i += 4)
// 		flush |= *(u32 *)((u8 *)th + i) ^
// 			 *(u32 *)((u8 *)th2 + i);

// 	/* When we receive our second frame we can made a decision on if we
// 	 * continue this flow as an atomic flow with a fixed ID or if we use
// 	 * an incrementing ID.
// 	 */
// 	if (NAPI_GRO_CB(p)->flush_id != 1 ||
// 	    NAPI_GRO_CB(p)->count != 1 ||
// 	    !NAPI_GRO_CB(p)->is_atomic)
// 		flush |= NAPI_GRO_CB(p)->flush_id;
// 	else
// 		NAPI_GRO_CB(p)->is_atomic = false;

// 	mss = skb_shinfo(p)->gso_size;

// 	flush |= (len - 1) >= mss;
// 	flush |= (ntohl(th2->seq) + skb_gro_len(p)) ^ ntohl(th->seq);
// #ifdef CONFIG_TLS_DEVICE
// 	flush |= p->decrypted ^ skb->decrypted;
// #endif

// 	if (flush || skb_gro_receive(p, skb)) {
// 		mss = 1;
// 		goto out_check_final;
// 	}

// 	tcp_flag_word(th2) |= flags & (TCP_FLAG_FIN | TCP_FLAG_PSH);

// out_check_final:
// 	flush = len < mss;
// 	flush |= (__force int)(flags & (TCP_FLAG_URG | TCP_FLAG_PSH |
// 					TCP_FLAG_RST | TCP_FLAG_SYN |
// 					TCP_FLAG_FIN));

// 	if (p && (!NAPI_GRO_CB(skb)->same_flow || flush))
// 		pp = p;

// out:
// 	NAPI_GRO_CB(skb)->flush |= (flush != 0);

// 	return pp;

  struct sk_buff *pp = NULL;
	struct sk_buff *p, *p2, *p3, *p4, *p_next;
	struct sk_buff **hhead = &(container_of(head, struct sk_buff, list));
	struct tcphdr *th;
	struct tcphdr *th2;
	__u32 seq;
	__u32 seq2;
	__u32 len;
	__u32 len2;
	__u32 seq_next;
	__u32 seq_next2;
	__u32 in_seq;
	__u32 in_seq_next;
	unsigned int thlen;
	__be32 flags;
	unsigned int hlen;
	unsigned int off;
	int flush = 1;
	struct sk_buff_head_gro *ofo_queue;
	int err;

	NAPI_GRO_CB(skb)->out_of_order_queue = NULL;
	NAPI_GRO_CB(skb)->is_tcp = true;

	//printk(KERN_NOTICE "tcp0\n");
	//printk(KERN_NOTICE "%x\n", *head);

	off = skb_gro_offset(skb);
	hlen = off + sizeof(*th);
	th = skb_gro_header_fast(skb, off);
	if (skb_gro_header_hard(skb, hlen)) {
		th = skb_gro_header_slow(skb, hlen, off);
		if (unlikely(!th)) {
			NAPI_GRO_CB(skb)->flush = 1;
			return NULL;
		}
	}
	//printk(KERN_NOTICE "tcp1\n");

	thlen = th->doff * 4;
	if (thlen < sizeof(*th)) {
		NAPI_GRO_CB(skb)->flush = 1;
		return NULL;
	}
	//printk(KERN_NOTICE "tcp2\n");

	hlen = off + thlen;
	if (skb_gro_header_hard(skb, hlen)) {
		th = skb_gro_header_slow(skb, hlen, off);
		if (unlikely(!th)) {
			NAPI_GRO_CB(skb)->flush = 1;
			return NULL;
		}
	}
	//printk(KERN_NOTICE "tcp3\n");

	skb_gro_pull(skb, thlen);

	seq = ntohl(th->seq);
	len = skb_gro_len(skb);
	seq_next = seq + len;
	flags = tcp_flag_word(th);

	NAPI_GRO_CB(skb)->count = 1;
	NAPI_GRO_CB(skb)->seq = seq;
	NAPI_GRO_CB(skb)->len = len;
	NAPI_GRO_CB(skb)->tcp_hash = *(__u32 *)&th->source;

	for (; (p = *hhead); hhead = &p->next) {
		if (!NAPI_GRO_CB(p)->same_flow)
			continue;

		th2 = tcp_hdr(p);
		//printk(KERN_NOTICE "%x\n", *(u32 *)&th->source);
		//printk(KERN_NOTICE "%x\n", *(u32 *)&th2->source);

		if (*(u32 *)&th->source ^ *(u32 *)&th2->source) {
			NAPI_GRO_CB(p)->same_flow = 0;
			continue;
		}

		goto found;
	}

	// printk(KERN_INFO "queue not found\n");
	flush = len < 1;
	flush |= (__force int)(flags & (TCP_FLAG_URG |
					TCP_FLAG_RST | TCP_FLAG_SYN |
					TCP_FLAG_FIN));
	flush |= off < skb_headlen(skb);
	NAPI_GRO_CB(skb)->flush |= (flush != 0);
	return NULL;

found:
	//printk(KERN_NOTICE "found0\n");
	/* Include the IP ID check below from the inner most IP hdr */
	//printk(KERN_NOTICE "flush-1 %u\n", flush);
	//printk(KERN_NOTICE "flush-2 %u\n", NAPI_GRO_CB(p)->flush);
	//printk(KERN_NOTICE "flush-3 %u\n", NAPI_GRO_CB(p)->flush_id);
	flush = NAPI_GRO_CB(p)->flush;
	// if (flush)
	// printk(KERN_NOTICE "flush0 %u\n", flush);
	flush |= (__force int)(flags & (TCP_FLAG_RST | TCP_FLAG_SYN | TCP_FLAG_FIN | TCP_FLAG_URG));
	// if (flush)
	// printk(KERN_NOTICE "flush1 %u\n", flush);
	flush |= off < skb_headlen(skb);
	// if (flush)
	// printk(KERN_NOTICE "flush2 offset %u headlen %u\n", off, skb_headlen(skb));
	//flush |= (__force int)(th->ack_seq ^ th2->ack_seq);
	//if (flush)
	//printk(KERN_NOTICE "flush3 %u\n", flush);
	//for (i = sizeof(*th); i < thlen; i += 4)
	//	  flush |= *(u32 *)((u8 *)th + i) ^
	//		   *(u32 *)((u8 *)th2 + i);
	//if (flush)
	//printk(KERN_NOTICE "flush4 %u\n", flush);

	//mss = tcp_skb_mss(p);

	//flush |= (len - 1) >= mss;
	//if (flush)
	//printk(KERN_NOTICE "flush5 %u %u\n", len, mss);
	/* allow out of order packets to be merged latter */
	//flush |= (ntohl(th2->seq) + skb_gro_len(p)) ^ ntohl(th->seq);

	/*
	if (flush || skb_gro_receive(*hhead, skb)) {
		mss = 1;
		goto out_check_final;
	}*/

	ofo_queue = NAPI_GRO_CB(p)->out_of_order_queue;
	NAPI_GRO_CB(skb)->out_of_order_queue = ofo_queue;
	skb_shinfo(skb)->gso_size = len;
	//printk(KERN_NOTICE "%u\n", flush);
	//printk(KERN_NOTICE "%u\n", ofo_queue->qlen);

	if (flush) {
		// printk(KERN_NOTICE "flush point 1\n");
		NAPI_GRO_CB(skb)->flush = 1;
		return *hhead;
	}
	//printk(KERN_NOTICE "found1\n");

	if (before(seq, ofo_queue->seq_next)) {
		if (ofo_queue->flushed) {
			// printk(KERN_NOTICE "flush point 2\n");
			NAPI_GRO_CB(skb)->flush = 1;
			return NULL;
		} else {
			ofo_queue->seq_next = seq;
		}
	}

	// need to make sure the one in gro_list is always the *hhead of the ofo_queue
	//printk(KERN_NOTICE "enqueue\n");
	//NAPI_GRO_CB(skb)->age = jiffies;
	//NAPI_GRO_CB(skb)->timestamp = ktime_to_ns(ktime_get());
	p2 = NAPI_GRO_CB(p)->out_of_order_queue->next;
	p_next = p->next;
	in_seq_next = ofo_queue->seq_next;
	while (p2) {
		seq2 = NAPI_GRO_CB(p2)->seq;
		len2 = NAPI_GRO_CB(p2)->len;
		seq_next2 = seq2 + len2;

		in_seq = in_seq_next;
		if (in_seq == seq2) {
			in_seq_next = seq_next2;
		}
		//printk(KERN_NOTICE "seq %u\n", seq);
		//printk(KERN_NOTICE "seq_next %u\n", seq_next);
		//printk(KERN_NOTICE "seq2 %u\n", seq2);
		//printk(KERN_NOTICE "seq_next2 %u\n", seq_next2);


		if (before(seq_next, seq2)) {
			//printk(KERN_NOTICE "enqueue0\n");
			if ((flags & TCP_FLAG_PSH) && (seq == in_seq)) {
				p3 = NAPI_GRO_CB(p2)->prev;
				if (p3 != NULL) {
					*hhead = p2;
					p2->next = p_next;
					skb_gro_flush(ofo_queue, p3);
					// printk(KERN_INFO "flush point 100\n");
				}
				NAPI_GRO_CB(skb)->flush = 1;
				return NULL;
			} else {
				NAPI_GRO_CB(skb)->out_of_order_queue = ofo_queue;
				NAPI_GRO_CB(skb)->prev = NAPI_GRO_CB(p2)->prev;
				NAPI_GRO_CB(skb)->next = p2;
				ofo_queue->qlen += len;
				ofo_queue->skb_num++;
				if (NAPI_GRO_CB(p2)->prev == NULL) {
					ofo_queue->next = skb;
					NAPI_GRO_CB(p2)->prev = skb;

					*hhead = skb;
					skb->next = p_next;
				} else {
					NAPI_GRO_CB(NAPI_GRO_CB(p2)->prev)->next = skb;
					NAPI_GRO_CB(p2)->prev = skb;
				}

				NAPI_GRO_CB(skb)->same_flow = 1;
				return NULL;
			}

		} else if (seq_next == seq2) {
			//printk(KERN_NOTICE "enqueue1\n");

			if ((err = skb_gro_merge(skb, p2))) {
				if (err == SKB_MERGE_INVAL) {
					// printk(KERN_NOTICE "flush point 140\n");
					NAPI_GRO_CB(skb)->flush = 1;
					return *hhead;
				} else if (in_seq != seq) {
					NAPI_GRO_CB(skb)->out_of_order_queue = ofo_queue;
					NAPI_GRO_CB(skb)->prev = NAPI_GRO_CB(p2)->prev;
					NAPI_GRO_CB(skb)->next = p2;
					ofo_queue->qlen += len;
					ofo_queue->skb_num++;
					if (NAPI_GRO_CB(p2)->prev == NULL) {
						ofo_queue->next = skb;
						NAPI_GRO_CB(p2)->prev = skb;

						*hhead = skb;
						skb->next = p_next;
					} else {
						NAPI_GRO_CB(NAPI_GRO_CB(p2)->prev)->next = skb;
						NAPI_GRO_CB(p2)->prev = skb;
					}

					NAPI_GRO_CB(skb)->same_flow = 1;
					return NULL;

				} else if (err == SKB_MERGE_2BIG || tcp_hdr(p2)->psh || NAPI_GRO_CB(p2)->gso_end) {
					// printk(KERN_NOTICE "flush point 3\n");
					p3 = NAPI_GRO_CB(p2)->next;
					if (p3 != NULL) {
						// printk(KERN_NOTICE "flush point 31\n");
						*hhead = p3;
						p3->next = p_next;
						skb_gro_flush(ofo_queue, p2);
						NAPI_GRO_CB(skb)->flush = 1;
						return NULL;
					} else {
						// printk(KERN_NOTICE "flush point 32\n");
						NAPI_GRO_CB(skb)->flush = 1;
						return *hhead;
					}
				} else {
					// printk(KERN_NOTICE "flush point 30\n");
					p3 = NAPI_GRO_CB(p2)->prev;
					if (p3 != NULL) {
						*hhead = p2;
						p2->next = p_next;
						skb_gro_flush(ofo_queue, p3);
					}
					NAPI_GRO_CB(skb)->flush = 1;
					return NULL;
				}
			}

			NAPI_GRO_CB(skb)->out_of_order_queue = ofo_queue;
			NAPI_GRO_CB(skb)->prev = NAPI_GRO_CB(p2)->prev;
			NAPI_GRO_CB(skb)->next = NAPI_GRO_CB(p2)->next;
			//NAPI_GRO_CB(skb)->age = NAPI_GRO_CB(p2)->age;
			//NAPI_GRO_CB(skb)->timestamp = NAPI_GRO_CB(p2)->timestamp;
			ofo_queue->qlen += len;

			if (NAPI_GRO_CB(p2)->prev == NULL) {
				ofo_queue->next = skb;

				*hhead = skb;
				skb->next = p_next;
			} else {
				NAPI_GRO_CB(NAPI_GRO_CB(p2)->prev)->next = skb;
			}

			if (NAPI_GRO_CB(p2)->next == NULL) {
				ofo_queue->prev = skb;
			} else {
				NAPI_GRO_CB(NAPI_GRO_CB(p2)->next)->prev = skb;
			}

			skb_gro_free(p2);

			NAPI_GRO_CB(skb)->same_flow = 1;
			break;

		} else if (seq == seq_next2) {
			//printk(KERN_NOTICE "enqueue2\n");

			if ((err = skb_gro_merge(p2, skb))) {

				if (err != SKB_MERGE_INVAL) {
					p3 = NAPI_GRO_CB(p2)->next;
					if (p3 != NULL) {
						if (in_seq == seq2) {
							// printk(KERN_NOTICE "flush point 40\n");
							*hhead = p3;
							p3->next = p_next;
							skb_gro_flush(ofo_queue, p2);
						}
						p2 = p3;
						continue;
					} else {
						if (in_seq == seq2 && th->psh) {
							// printk(KERN_NOTICE "flush point 110\n");
							NAPI_GRO_CB(skb)->flush = 1;
							return *hhead;
						}

						NAPI_GRO_CB(skb)->out_of_order_queue = ofo_queue;
						NAPI_GRO_CB(skb)->prev = p2;
						NAPI_GRO_CB(skb)->next = NULL;

						ofo_queue->qlen += len;
						ofo_queue->skb_num++;

						NAPI_GRO_CB(p2)->next = skb;
						ofo_queue->prev = skb;

						NAPI_GRO_CB(skb)->same_flow = 1;

						if (in_seq == seq2) {
							// printk(KERN_NOTICE "flush point 41\n");
							*hhead = skb;
							skb->next = p_next;
							skb_gro_flush(ofo_queue, p2);
						}

						return NULL;
					}

				} else {
					// printk(KERN_NOTICE "flush point 4\n");
					NAPI_GRO_CB(skb)->flush = 1;
					return *hhead;
				}
			}

			ofo_queue->qlen += len;
			th2 = tcp_hdr(p2);
			p3 = NAPI_GRO_CB(p2)->next;
			if (in_seq == seq2 && th2->psh) {
				if (p3 == NULL) {
					// printk(KERN_NOTICE "flush point 120\n");
					return *hhead;
				} else {
					// printk(KERN_NOTICE "flush point 101\n");
					*hhead = p3;
					p3->next = p_next;
					skb_gro_flush(ofo_queue, p2);
					return NULL;
				}
			}

			if (p3 != NULL) {

				if (seq_next == NAPI_GRO_CB(p3)->seq) {

					//printk(KERN_NOTICE "merge p3\n");
					if ((err = skb_gro_merge(p2, p3))) {
						if (err != SKB_MERGE_INVAL) {
							if (in_seq == seq2) {
								// printk(KERN_NOTICE "flush point 50\n");
								*hhead = p3;
								p3->next = p_next;
								skb_gro_flush(ofo_queue, p2);
							}
							return NULL;
						} else {
							//printk(KERN_NOTICE "merge fail\n");
							// printk(KERN_NOTICE "flush point 5\n");
							return *hhead;
						}
					}

					ofo_queue->skb_num--;

					NAPI_GRO_CB(p2)->next = NAPI_GRO_CB(p3)->next;
					//NAPI_GRO_CB(p2)->age = min(NAPI_GRO_CB(p2)->age, NAPI_GRO_CB(p3)->age);
					//NAPI_GRO_CB(p2)->timestamp = min(NAPI_GRO_CB(p2)->timestamp, NAPI_GRO_CB(p3)->timestamp);

					skb_gro_free(p3);

					if (NAPI_GRO_CB(p2)->next == NULL) {
						ofo_queue->prev = p2;
					} else {
						NAPI_GRO_CB(NAPI_GRO_CB(p2)->next)->prev = p2;
					}

					p3 = NAPI_GRO_CB(p2)->next;
					if (in_seq == seq2 && th2->psh) {
						if (p3 == NULL) {
							// printk(KERN_NOTICE "flush point 130\n");
							return *hhead;
						} else {
							// printk(KERN_NOTICE "flush point 102\n");
							*hhead = p3;
							p3->next = p_next;
							skb_gro_flush(ofo_queue, p2);
							return NULL;
						}
					}

				} else if (after(seq_next, NAPI_GRO_CB(p3)->seq)) {
					// printk(KERN_NOTICE "flush point 7\n");
					return *hhead;
				}

			}

			return NULL;

		} else if (after(seq, seq_next2)) {
			//printk(KERN_NOTICE "enqueue3\n");

			if (NAPI_GRO_CB(p2)->next == NULL) {

				NAPI_GRO_CB(skb)->out_of_order_queue = ofo_queue;
				NAPI_GRO_CB(skb)->prev = p2;
				NAPI_GRO_CB(skb)->next = NULL;

				ofo_queue->qlen += len;
				ofo_queue->skb_num++;
				ofo_queue->prev = skb;
				NAPI_GRO_CB(p2)->next = skb;

				NAPI_GRO_CB(skb)->same_flow = 1;
				break;

			} else {
				p2 = NAPI_GRO_CB(p2)->next;
				continue;
			}

		} else {
			//printk(KERN_NOTICE "enqueue4\n");
			// printk(KERN_NOTICE "flush point 8\n");
			NAPI_GRO_CB(skb)->flush = 1;
			return *hhead;
		}
	}

	return NULL;
}

int tcp_gro_complete(struct sk_buff *skb)
{
	struct tcphdr *th = tcp_hdr(skb);

	skb->csum_start = (unsigned char *)th - skb->head;
	skb->csum_offset = offsetof(struct tcphdr, check);
	skb->ip_summed = CHECKSUM_PARTIAL;

	skb_shinfo(skb)->gso_segs = NAPI_GRO_CB(skb)->count;

	if (th->cwr)
		skb_shinfo(skb)->gso_type |= SKB_GSO_TCP_ECN;

	return 0;
}
EXPORT_SYMBOL(tcp_gro_complete);

static struct sk_buff *tcp4_gro_receive(struct list_head *head, struct sk_buff *skb)
{
	/* Don't bother verifying checksum if we're going to flush anyway. */
	if (!NAPI_GRO_CB(skb)->flush &&
	    skb_gro_checksum_validate(skb, IPPROTO_TCP,
				      inet_gro_compute_pseudo)) {
		NAPI_GRO_CB(skb)->flush = 1;
		return NULL;
	}

	return tcp_gro_receive(head, skb);
}

static int tcp4_gro_complete(struct sk_buff *skb, int thoff)
{
	const struct iphdr *iph = ip_hdr(skb);
	struct tcphdr *th = tcp_hdr(skb);

	th->check = ~tcp_v4_check(skb->len - thoff, iph->saddr,
				  iph->daddr, 0);
	skb_shinfo(skb)->gso_type |= SKB_GSO_TCPV4;

	if (NAPI_GRO_CB(skb)->is_atomic)
		skb_shinfo(skb)->gso_type |= SKB_GSO_TCP_FIXEDID;

	return tcp_gro_complete(skb);
}

static const struct net_offload tcpv4_offload = {
	.callbacks = {
		.gso_segment	=	tcp4_gso_segment,
		.gro_receive	=	tcp4_gro_receive,
		.gro_complete	=	tcp4_gro_complete,
	},
};

int __init tcpv4_offload_init(void)
{
	return inet_add_offload(&tcpv4_offload, IPPROTO_TCP);
}
