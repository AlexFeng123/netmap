/*
 * Copyright (C) 2015 NetApp. Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * common headers
 */
#if defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/errno.h>
#include <sys/param.h>	/* defines used in kernel.h */
#include <sys/kernel.h>	/* types used in module initialization */
#include <sys/conf.h>	/* cdevsw struct, UID, GID */
#include <sys/sockio.h>
#include <sys/socketvar.h>	/* struct socket */
#include <sys/malloc.h>
#include <sys/poll.h>
#include <sys/rwlock.h>
#include <sys/socket.h> /* sockaddrs */
#include <sys/selinfo.h>
#include <sys/sysctl.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/bpf.h>		/* BIOCIMMEDIATE */
#include <machine/bus.h>	/* bus_dmamap_* */
#include <sys/endian.h>
#include <sys/refcount.h>

#elif defined(linux)
#include <bsd_glue.h>
#endif

#include <net/netmap.h>
#include <dev/netmap/netmap_kern.h>
#include <dev/netmap/netmap_mem2.h>
#include <dev/netmap/netmap_bdg.h>

#ifdef WITH_STACK
#define NM_STACKMAP_PULL 1
//#define STACKMAP_COPY 1
static int stackmap_mode = NM_STACKMAP_PULL;//NM_STACKMAP_PULL
SYSBEGIN(vars_stack);
SYSCTL_DECL(_dev_netmap);
SYSCTL_INT(_dev_netmap, OID_AUTO, stackmap_mode, CTLFLAG_RW, &stackmap_mode, 0 , "");
SYSEND;

static inline struct netmap_adapter *
stackmap_master(const struct netmap_adapter *slave)
{
	struct netmap_vp_adapter *vpna;

	if (!slave)
		return NULL;
	vpna = (struct netmap_vp_adapter *)slave;
	return &netmap_bdg_port(vpna->na_bdg, 0)->up;
}

static inline int
stackmap_is_host(struct netmap_adapter *na)
{
	return na->nm_register == NULL;
}

/* nm_notify() for NIC RX */
static int
stackmap_intr_notify(struct netmap_kring *kring, int flags)
{
	struct netmap_adapter *na, *vpna, *mna;
	enum txrx t = NR_RX;

	KASSERT(kring, "kring is NULL");
	na = kring->na;
	vpna = (struct netmap_adapter *)na->na_private;
	if (!vpna)
		return NM_IRQ_COMPLETED;

	/* maybe TX */
	if (kring >= NMR(na, NR_TX) &&
	    kring <= NMR(na, NR_TX) + na->num_tx_rings) {
		t = NR_TX;
	}
	ND("%s interrupt (kring %p)", t == NR_TX ? "tx" : "rx", kring);

	/* just wakeup the client on the master */
	mna = stackmap_master(vpna);
	if (mna) {
		struct netmap_kring *mkring;
		u_int me = kring - NMR(na, t), mnr;

		if (stackmap_mode != NM_STACKMAP_PULL) {
			D("calling bwrap_intr_notify");
			return netmap_bwrap_intr_notify(kring, flags);
		}
		mnr = t == NR_RX ? mna->num_rx_rings : mna->num_tx_rings;
		mkring = &NMR(mna, t)[mnr > me ? me : 0];
		KASSERT(mkring->nm_notify, "nm_notify is null at mna");
		ND("waking up master %s", mna->name);
		mkring->nm_notify(mkring, 0);
	}
	return NM_IRQ_COMPLETED;
}

/* Stackmap version of the flush routine.
 * We ask the stack to identify destination NIC.
 * Packets are moved around by buffer swapping
 * Unsent packets throttle the source ring
 * Packets are re-iterated after dst->notify, 
 * TODO currently just forward all the packets
 * TODO consider if we can merge this with the original flush routine.
 */
enum {
	NM_STACK_CONSUMED=0,
	NM_STACK_DEFERRED,
	NM_STACK_CONSUMED_RESERVING,
};


/* also fine for non extra slots */
static inline void
stackmap_extra_dequeue(struct netmap_slot *slot)
{
	slot->len = slot->offset = slot->next = 0;
	slot->fd = 0;
	if (slot->flags & NS_STUCK) {
		//D("stuck solved");
		slot->flags &= ~NS_STUCK;
		slot->flags |= NS_RELEASED;
	}
}

void
stackmap_mbuf_destructor(struct mbuf *m)
{
	struct stackmap_sk_adapter *ska;
	struct stackmap_cb *scb = STACKMAP_CB(m);

	KASSERT(m->sk, "m->sk is NULL!");
	ska = stackmap_sk(m->sk);
	if (ska) {
		int was_stuck = (scb->slot->flags & NS_STUCK);
		stackmap_extra_dequeue(scb->slot);
		if (was_stuck)
			D("m %p buf %p stuck solved", m, m->head);
	}
	if ((m->destructor = scb->save_mbuf_destructor) != NULL) {
		m->destructor(m);
		scb->save_mbuf_destructor = NULL;
	}
	KASSERT(!(scb->flags & (SCB_M_ORIGIN | SCB_M_TRANSMIT)),
			("wird flags 0x%x", scb->flags));
	scb->flags &= ~SCB_M_QUEUED; // netmap-originated skb

	D("m %p %s", m, ska ?  "unlinked from extra"
	       	: "socket has already been unregistered");
}

#define STACKMAP_FD_HOST	(NM_BDG_MAXPORTS*NM_BDG_MAXRINGS-1)

/* TX:
 * 1. sort packets by socket with forming send buffer (in-order iteration)
 * 2. do tcp processing on each socket (out-of-order iteration)
 * We must take into account MOREFRAGS.
 * We do not support INDIRECT as packet movement is done by swapping
 * We thus overwrite ptr field (8 byte width) in a slot to store a 
 * socket (4 byte), next buf index (2 byte).
 * The rest of 2 bytes may be used to store the number of frags 
 * (1 byte) and destination port (1 byte).
 */

static inline struct nm_bdg_q *
stackmap_fdtable(struct nm_bdg_fwd *ft)
{
	return (struct nm_bdg_q *)(ft + NM_BDG_BATCH_MAX);
}

static inline uint16_t *
stackmap_nfdsp(struct nm_bdg_q *fdtable)
{
	return (uint16_t *)
		(fdtable + NM_BDG_MAXPORTS * NM_BDG_MAXRINGS);
}

/* Called while walking the receive skb queue,
 * so guaranteed to be in-order
 */
void
stackmap_add_fdtable(struct stackmap_cb *scb, char *buf)
{
	struct netmap_kring *kring;
	struct nm_bdg_fwd *ft, *ft_p;
	u_int fd_i, i;
	int32_t *fds;
	struct nm_bdg_q *fde;
	uint16_t *nfds, *npkts;

	/* obtain ft position */
	kring = scb->kring; // kring might differ even in a socket?
	ft = kring->nkr_ft;

	fde = (struct nm_bdg_q *)(ft + NM_BDG_BATCH_MAX);
	nfds = (uint16_t *)(fde + NM_BDG_MAXPORTS * NM_BDG_MAXRINGS);
	npkts = nfds + 1; // bq_tail
	fds = (int32_t *)
		(fde + NM_BDG_MAXPORTS * NM_BDG_MAXRINGS + 1);
	i = kring->nkr_ft_cur++;
	ft_p = ft + i;
	if (kring->nkr_ft_cur > NM_BDG_BATCH_MAX) {
		D("ft full");
		return;
	}
	ND("ft %p i %d", ft, i);

	/* bring the packet to ft */
	ft_p->ft_buf = buf;
#define ft_offset	_ft_port
	ft_p->ft_offset = scb->slot->offset;
	ft_p->ft_len = scb->slot->len;
	ft_p->ft_flags = 0; // for what?
	ft_p->ft_next = NM_FT_NULL;
	fd_i = scb->slot->fd;

	ND("ft_cur %d fd %u, nfds %d len %d off %d", ft_p - ft, fd_i, *nfds, ft_p->ft_len, ft_p->ft_offset);

	/* add to fdtable */
	fde += fd_i;
	if (fde->bq_head == NM_FT_NULL) {
		fde->bq_head = fde->bq_tail = i;
		fds[(*nfds)++] = fd_i;
		ND("new entry %d (nfds %d)", fd_i, *nfds);
	} else {
		ND("existing entry %d (head %d nfds %d)", fd_i, fde->bq_head, *nfds);
		ft[fde->bq_tail].ft_next = i;
		fde->bq_tail = i;
	}
	(*npkts)++;
#undef ft_offset
}

struct stackmap_sk_adapter *
stackmap_ska_from_fd(struct netmap_adapter *na, int fd)
{
	struct stackmap_adapter *sna = (struct stackmap_adapter *)na;
	struct stackmap_sk_adapter *ska, *tmp;

	(void)tmp;
	NM_LIST_FOREACH_SAFE(ska, &sna->sk_adapters, next, tmp) {
		if (ska->fd == fd)
			break;
	}
	return ska;
}

static int stackmap_bdg_flush(struct netmap_kring *kring)
{
	int k = kring->nr_hwcur, j;
	const int rhead = kring->rhead;
	u_int lim_tx = kring->nkr_num_slots - 1;
	struct netmap_adapter *na = kring->na;
	struct netmap_vp_adapter *vpna =
		(struct netmap_vp_adapter *)na;
	struct netmap_adapter *rxna;
	struct nm_bdg_fwd *ft = kring->nkr_ft;
	int32_t *fds;
	struct nm_bdg_q *fd_ents;
	uint16_t *nfds, *npkts;
	int32_t i, needed, lim_rx;
	uint32_t my_start = 0, lease_idx = 0, howmany;
	u_int dring;
	struct netmap_kring *rxkring;
	struct netmap_ring *rxring;
	u_int num_fds;
	int rx = 0;
	int host = 0;

	/* We use the broadcast entry to store the number of 
	 * fds (bq_head) and packets (bq_tail)
	 * We also use the last unicast entry to indicate the
	 * host stack
	 */

	fd_ents = stackmap_fdtable(ft);
	nfds = stackmap_nfdsp(fd_ents);
	npkts = nfds + 1; // bq_tail
	*nfds = *npkts = 0;
	kring->nkr_ft_cur = 0;
	fds = (int32_t *)(fd_ents +
		NM_BDG_MAXPORTS * NM_BDG_MAXRINGS + 1);

	if (netmap_bdg_rlock(vpna->na_bdg, na)) {
		RD(1, "failed to obtain rlock");
		return 0;
	}

	/* XXX perhaps this is handled later? */
	if (netmap_bdg_active_ports(vpna->na_bdg) < 2) {
		RD(1, "only %d active ports",
				netmap_bdg_active_ports(vpna->na_bdg));
		goto unlock_out;
	}

	/* TODO stack must decide destination */
	if (na == stackmap_master(na)) {
		rxna = &netmap_bdg_port(vpna->na_bdg, 1)->up;
	} else if (unlikely(na ==
		       	&((struct netmap_bwrap_adapter *)
			na->na_private)->host.up)) /* from host */ {
		rxna = &netmap_bdg_port(vpna->na_bdg, 1)->up;
		host = 1;
	} else {
		rxna = stackmap_master(na);
		rx = 1;
	}

	/* examine all the slots */
	k = kring->nr_hwcur;
	//if (na != stackmap_master(na))
	//	D("%s: sending %d packets", na->name,
	 //   rhead > k ? rhead - k : rhead + lim_tx - k);
	for (j = k; j != rhead; j = j == lim_tx ? 0 : j + 1) {
		/* slot->len covers between DMA off to data */
		struct netmap_slot *slot = &kring->ring->slot[j];
		struct stackmap_cb *scb;
		struct mbuf *m;

		if (host) {
			char *buf = NMB(na, slot);

			slot->fd = STACKMAP_FD_HOST;
			scb = STACKMAP_CB_BUF(buf,
					NETMAP_BUF_SIZE(na));
			scb->slot = slot;
			scb->kring = kring;
			scb->save_mbuf_destructor = NULL; // why?
			//scb->flags = 0; // why clear?
			ND("adding buf %p type 0x%x off %d", buf,
			    ntohs(*(uint16_t *)(buf+14)), slot->offset);
			stackmap_add_fdtable(scb, buf);
			k = nm_next(k, lim_tx);
			continue;
		}

		/* The user has specified data length + offset for
		 * slot->len
		 * BTW, make sure to avoid duplicate allocation
		 */
		if (slot->flags & NS_RELEASED) {
			k = nm_next(k, lim_tx);
			slot->flags &= ~NS_RELEASED;
			continue;
		} else if (unlikely(slot->flags & NS_STUCK)) {
			struct page *page;

		       	page = virt_to_page(NMB(na, slot));
			scb = STACKMAP_CB_BUF(NMB(na, slot),
					NETMAP_BUF_SIZE(na));
			if (!stackmap_extra_enqueue(na, slot)) {
				k = nm_next(k, lim_tx);
				continue;
			}
			RD(1, "buf %p still stuck (pageref %d)",
				NMB(na, slot), page_ref_count(page));
			break; /* still in use, break */
		}
		scb = STACKMAP_CB_BUF(NMB(na, slot),
			NETMAP_BUF_SIZE(na));
		if (scb) {
			uint16_t type;

			type = ntohs(*(uint16_t *)(NMB(na, slot) + 14));
			if (scb->slot == slot)
				ND("%s: unfreed buf type 0x%x at slot %d (out of %d) ?", rx?"rx":"tx", type, j, lim_tx);
			//KASSERT(scb->slot != slot, ("previously mbuf allocated on %d?", k));
		}
		m = nm_os_build_mbuf(na, NMB(na, slot), slot->len);
		if (!m)
			break;
		/* m->end: beginning of shinfo
		 * m->tail: end of data/packet
		 * m->data: beginning of IP header
		 */
		scb = STACKMAP_CB(m);
		/* ToDo: Expensive, optimize it */
		bzero(scb, sizeof(*scb));
		scb->kring = kring;
		scb->slot = slot;

		if (rx) {
			nm_os_stackmap_mbuf_recv(m);
		} else {
			if (nm_os_stackmap_mbuf_send(m))
				break;
			/* know we know packet has been queued
			 * to one socket */
		}
		k = nm_next(k, lim_tx);
	}
	//if (!rx) {
	//	/* specific to TX, push pending packets */
	//	goto drain_out;
	//}
	/* TX: At this point we should see socket buffer enqueued */

	// ToDo examine num mismatch case */

	/* pass 2 with lock */

	if (unlikely(!nm_netmap_on(rxna)))
		goto unlock_out;
	dring = kring - NMR(kring->na, NR_TX);
	nm_bound_var(&dring, 0, 0, rxna->num_rx_rings, NULL);
	rxkring = &NMR(rxna, NR_RX)[dring];
	rxring = rxkring->ring;
	lim_rx = rxkring->nkr_num_slots - 1;

	needed = *npkts;
	*npkts = 0;
	mtx_lock(&rxkring->q_lock);
	if (rxkring->nkr_stopped) {
		mtx_unlock(&rxkring->q_lock);
		goto unlock_out;
	}
	my_start = j = rxkring->nkr_hwlease;
	howmany = nm_kr_space(rxkring, 1);
	if (howmany == 0)
		ND("rxring still full");
	if (needed < howmany)
		howmany = needed;
	lease_idx = nm_kr_lease(rxkring, howmany, 1);
	mtx_unlock(&rxkring->q_lock);

	num_fds = *nfds;
	*nfds = 0;

	for (i = 0; i < num_fds; i++) {
		struct nm_bdg_q *d;
		u_int next, d_i;

		d_i = fds[i];
		d = fd_ents + d_i;
		next = d->bq_head;

		do {
			struct nm_bdg_fwd *ft_p = ft + next;
			struct netmap_slot *ts, *rs, tmp;
			struct stackmap_cb *scb;

			next = ft_p->ft_next;

			scb = STACKMAP_CB_BUF(ft_p->ft_buf,
					NETMAP_BUF_SIZE(na));
			ts = scb->slot;

			/* ts already includes fd */
			rs = &rxkring->ring->slot[j];
			tmp = *rs;
			*rs = *ts;
			*ts = tmp;
			if (rs->buf_idx == 0)
				D("buf_idx 0 at %d", j);
			ts->flags |= NS_BUF_CHANGED;
			rs->flags |= NS_BUF_CHANGED;
			j = nm_next(j, lim_rx);
		} while (--howmany && next != NM_FT_NULL);
		{
		    /* current position */
		    uint32_t *p = rxkring->nkr_leases; /* shorthand */
		    uint32_t update_pos;
		    int still_locked = 1;
		    struct netmap_ring *ring = rxkring->ring;
	
		    mtx_lock(&rxkring->q_lock);
		    if (unlikely(howmany > 0)) {
			/* not used all bufs. If i am the last one
			 * i can recover the slots, otherwise must
			 * fill them with 0 to mark empty packets.
			 */
			ND("leftover %d bufs", howmany);
			if (nm_next(lease_idx, lim_rx) == rxkring->nkr_lease_idx) {
			    /* yes i am the last one */
			    ND("roll back nkr_hwlease to %d", j);
			    rxkring->nkr_hwlease = j;
			} else {
			    while (howmany-- > 0) {
				ring->slot[j].len = 0;
				ring->slot[j].flags = 0;
				j = nm_next(j, lim_rx);
			    }
			}
		    }
		    p[lease_idx] = j; /* report I am done */
		    update_pos = rxkring->nr_hwtail;
	
		    if (my_start == update_pos) {
			/* all slots before my_start have been reported,
			 * so scan subsequent leases to see if other ranges
			 * have been completed, and to a selwakeup or txsync.
		         */
			while (lease_idx != rxkring->nkr_lease_idx &&
				p[lease_idx] != NR_NOSLOT) {
			    j = p[lease_idx];
			    p[lease_idx] = NR_NOSLOT;
			    lease_idx = nm_next(lease_idx, lim_rx);
			}
			/* j is the new 'write' position. j != my_start
			 * means there are new buffers to report
			 */
			if (likely(j != my_start)) {
				rxkring->nr_hwtail = j;
				still_locked = 0;
				mtx_unlock(&rxkring->q_lock);
				rxkring->nm_notify(rxkring, 0);
				/* this is netmap_notify for VALE ports and
				 * netmap_bwrap_notify for bwrap. The latter will
				 * trigger a txsync on the underlying hwna
				 */
			}
		    }
		    if (still_locked)
			mtx_unlock(&rxkring->q_lock);
		}
		d->bq_head = d->bq_tail = NM_FT_NULL; /* cleanup */
		d->bq_len = 0;
	}

//drain_out:
	/* drain everything */
//	kring->nr_hwcur = k;
//	kring->nr_hwtail = nm_prev(k, lim_tx);
	/* cleanup */
	for (i = 0; i < NM_BDG_MAXPORTS * NM_BDG_MAXRINGS-1; i++) {
		fd_ents[i].bq_head = fd_ents[i].bq_tail = NM_FT_NULL;
	}
unlock_out:
	netmap_bdg_runlock(vpna->na_bdg);
	return k;
//	return 0;
}

static int
stackmap_txsync(struct netmap_kring *kring, int flags)
{
	struct netmap_adapter *na = kring->na;
	u_int const head = kring->rhead;
	u_int done;

	if (!((struct netmap_vp_adapter *)na)->na_bdg) {
		done = head;
		return 0;
	}
	done = stackmap_bdg_flush(kring);
	/* debug to drain everything */
	//kring->nr_hwcur = head;
	kring->nr_hwcur = done;
	//kring->nr_hwtail = nm_prev(head, kring->nkr_num_slots - 1);
	kring->nr_hwtail = nm_prev(done, kring->nkr_num_slots - 1);
	return 0;
}

/* rxsync for stackport */
static int
stackmap_rxsync(struct netmap_kring *kring, int flags)
{
	struct stackmap_adapter *sna = (struct stackmap_adapter *)kring->na;
	struct nm_bridge *b = sna->up.na_bdg;
	u_int me = kring - NMR(kring->na, NR_RX);
	int i;

	/* TODO scan only necessary ports */
	if (stackmap_mode == NM_STACKMAP_PULL) {
		for_bdg_ports(i, b) {
			struct netmap_vp_adapter *vpna = netmap_bdg_port(b, i);
			struct netmap_adapter *na = &vpna->up;
			struct netmap_adapter *hwna;
			struct netmap_kring *hwkring;
	
			if (netmap_bdg_idx(vpna) == netmap_bdg_idx(&sna->up))
				continue;
			else if (stackmap_is_host(na))
				continue;
			KASSERT(nm_is_bwrap(na), "no bwrap attached!");

			/* We assume the same number of hwna with vpna
			 * (see netmap_bwrap_attach()) */
			hwna = ((struct netmap_bwrap_adapter *)vpna)->hwna;
			hwkring = NMR(hwna, NR_RX) +
				(na->num_tx_rings > me ? me : 0);
			netmap_bwrap_intr_notify(hwkring, flags);
		}
	}
	return netmap_vp_rxsync(kring, flags);
}

int
stackmap_ndo_start_xmit(struct mbuf *m, struct ifnet *ifp)
{
	struct stackmap_cb *scb;
	/* this field has survived cloning */
	ND("m %p (type 0x%04x) %s destructor %p", m,
			ntohs(*(uint16_t *)(m->data+12)),
			m->wifi_acked_valid ? "direct, to fdtable" :
		       	"indirect, netmap_start_xmit()", stackmap_mbuf_destructor);
	if (!m->wifi_acked_valid) /* originated by the stack */ {
		ND("indirect transmit m %p (type 0x%x)",
		    m, ntohs(*(uint16_t *)(m->head + 14)));
		return linux_netmap_start_xmit(m, ifp);
	} else if (m->destructor == stackmap_mbuf_destructor) {
	       	/* originated by the stack but has been queued
		 * it might be from a slot */
		D("queued transmit m %p (type 0x%x)",
		    m, ntohs(*(uint16_t *)(m->head + 14)));
		netmap_transmit(ifp, m);
		return 0;
		/* we can skip copy */
		/*
		txr = MBUF_TXQ(m);
		tx_kring = &NMR(na, NR_TX)[txr];
		if (tx_kring->nr_mode == NKR_NETMAP_OFF) {
			return MBUF_TRANSMIT(na, ifp, m);
		}
		*/
	}

	scb = STACKMAP_CB(m);
	KASSERT(scb->flags & SCB_M_QUEUED, "strange - not SCB_M_QUEUED state");
	scb->flags &= ~SCB_M_QUEUED;
	scb->slot->len = MBUF_HEADLEN(m);
	stackmap_add_fdtable(scb, m->head);
	//kfree_skb(m);
	m_freem(m);
	return 0;
}

/* XXX Really ugly to separate from reg_slaves(), but we cannot detach
 * slaves by name as get_bnsbridges() fails due to lack of current.
 */
static void
stackmap_unreg_slaves(struct netmap_adapter *na) {
	struct stackmap_adapter *sna = (struct stackmap_adapter *)na;
	struct nm_bridge *b = sna->up.na_bdg;
	int i, me = netmap_bdg_idx(&sna->up);

	for_bdg_ports(i, b) {
		struct netmap_adapter *slave = &netmap_bdg_port(b, i)->up;
		struct netmap_adapter *hwna;
		struct lut_entry *lut;

		if (i == me)
			continue;
		hwna = ((struct netmap_bwrap_adapter *)slave)->hwna;
		lut = hwna->na_lut.lut;
		netmap_adapter_get(slave);
		slave->nm_bdg_ctl(slave, NULL, 0);
		/* restore default start_xmit for future register */
		((struct netmap_hw_adapter *)
		    hwna)->nm_ndo.ndo_start_xmit = linux_netmap_start_xmit;
		netmap_adapter_put(slave);
	}
	ND("done for slaves");
}

static int
stackmap_reg_slaves(struct netmap_adapter *na)
{
	struct stackmap_adapter *sna = (struct stackmap_adapter *)na;
	char *tok, *s, *s_orig;
	int error = 0;
	struct nmreq nmr;
	char *p = nmr.nr_name;

	bzero(&nmr, sizeof(nmr));
	nmr.nr_version = NETMAP_API;
	nmr.nr_cmd = NETMAP_BDG_ATTACH;
	/* Regular host stack port for indirect packets */
	nmr.nr_arg1 = NETMAP_BDG_HOST;
	p += strlcat(p, ":", sizeof(nmr.nr_name) - 
		strlcpy(p, netmap_bdg_name(&sna->up), sizeof(nmr.nr_name)));
	if (!sna->suffix)
		return 0;

	s = strdup(sna->suffix, M_DEVBUF);
	if (!s)
		return ENOMEM;
	s_orig = s;
	while ((tok = strsep(&s, "+")) != NULL &&
	    strncmp(tok, p, strlen(tok))) {
		struct netmap_adapter *slave = NULL;
		struct netmap_bwrap_adapter *bna;
		struct netmap_adapter *hwna;
		struct netmap_hw_adapter *h;
		struct netmap_adapter *vpna;
		int i;

		strlcpy(p, tok, strlen(tok) + 1);
		ND("registering %s (tok %s)", nmr.nr_name, tok);
		error = netmap_get_bdg_na(&nmr, &slave, na->nm_mem, 1);
		if (error)
			continue;
		if (!slave || !nm_is_bwrap(slave) /* XXX ugly */) {
			D("no error on get_bdg_na() but no valid adapter");
			netmap_adapter_put(slave);
			continue;
		}

		bna = (struct netmap_bwrap_adapter *)slave;
		vpna = &bna->up.up;
		hwna = bna->hwna;

		/* For the first slave now it is the first time to have ifp
		 * We must set buffer offset before finalizing at nm_bdg_ctl()
		 * callback. As we see, we adopt the value for the first NIC */
		if (!na->virt_hdr_len) {
			na->virt_hdr_len = nm_os_hw_headroom(hwna->ifp);
			netmap_mem_set_buf_offset(na->nm_mem, na->virt_hdr_len);
			vpna->virt_hdr_len = na->virt_hdr_len; /* for RX path */
		}
		KASSERT(na->virt_hdr_len == 2, ("virt_hdr_len %u!\n", na->virt_hdr_len));

		KASSERT(na->nm_mem == slave->nm_mem, "slave has different mem");
		error = slave->nm_bdg_ctl(slave, &nmr, 1);
		if (error) {
			netmap_adapter_put(slave);
			continue;
		}

		/* we don't have keep original intr_notify() as
		 * we do this after original reg callback
		 */
		if (stackmap_mode == NM_STACKMAP_PULL) {
			for (i = 0; i < hwna->num_rx_rings; i++) {
				hwna->rx_rings[i].nm_notify = 
					stackmap_intr_notify;
			}
			/*
			for (i = 0; i < hwna->num_tx_rings; i++) {
				hwna->tx_rings[i].nm_notify =
					stackmap_intr_notify;
			}
			*/
		}
		for (i = 0; i < vpna->num_tx_rings; i++)
			vpna->tx_rings[i].nm_sync = stackmap_txsync;
		/* packets originated by the host stack
		 * simply go into the bridge
		 */
		if (bna->host.na_bdg) {
			vpna->tx_rings[i].nm_sync = stackmap_txsync;
		}

		/* na->if_transmit already has backup */
		h = (struct netmap_hw_adapter *)hwna;
		h->nm_ndo.ndo_start_xmit =
			stackmap_ndo_start_xmit;
		/* re-overwrite */
		hwna->ifp->netdev_ops = &h->nm_ndo;
	}
	nm_os_free(s_orig);
	return error;
}

/*
 * When stackmap dies first, it simply restore all the socket
 * information on dtor().
 * Otherwise our sk->sk_destructor will cleanup stackmap states
 */
static void
stackmap_unregister_socket(struct stackmap_sk_adapter *ska)
{
	NM_SOCK_T *sk = ska->sk;

	NM_LIST_DEL(ska, next);
	NM_SOCK_LOCK(sk);
	RESTORE_DATA_READY(sk, ska);
	RESTORE_DESTRUCTOR(sk, ska);
	stackmap_wsk(NULL, sk);
	NM_SOCK_UNLOCK(sk);
	nm_os_free(ska);
	D("unregistered fd %d", ska->fd);
}

static void
stackmap_sk_destruct(NM_SOCK_T *sk)
{
	struct stackmap_sk_adapter *ska;
	struct stackmap_adapter *sna;

	ska = stackmap_sk(sk);
	if (ska->save_sk_destruct) {
		ska->save_sk_destruct(sk);
	}
	sna = (struct stackmap_adapter *)ska->na;
	netmap_bdg_wlock(sna->up.na_bdg);
	stackmap_unregister_socket(ska);
	netmap_bdg_wunlock(sna->up.na_bdg);
	D("unregistered socket");
}


static int
stackmap_register_fd(struct netmap_adapter *na, int fd)
{
	NM_SOCK_T *sk;
	struct stackmap_sk_adapter *ska;
	struct stackmap_adapter *sna = (struct stackmap_adapter *)na;

	sk = nm_os_sock_fget(fd);
	if (!sk)
		return EINVAL;

	ska = nm_os_malloc(sizeof(*ska));
	if (!ska) {
		nm_os_sock_fput(sk);
		return ENOMEM;
	}
	SAVE_DATA_READY(sk, ska);
	SAVE_DESTRUCTOR(sk, ska);
	ska->na = na;
	ska->sk = sk;
	ska->fd = fd;
	SET_DATA_READY(sk, nm_os_stackmap_data_ready);
	SET_DESTRUCTOR(sk, stackmap_sk_destruct);
	stackmap_wsk(ska, sk);
	NM_LIST_ADD(&sna->sk_adapters, ska, next);

	nm_os_sock_fput(sk);
	D("registered fd %d sk %p ska %p", fd, sk, ska);
	return 0;
}

static void
stackmap_bdg_dtor(const struct netmap_vp_adapter *vpna)
{
	struct stackmap_adapter *tmp, *sna;
	struct stackmap_sk_adapter *ska;

	if (&vpna->up != stackmap_master(&vpna->up))
		return;

	sna = (struct stackmap_adapter *)vpna;
	/* XXX Is this safe to remove entry ? */
	(void)tmp;
	NM_LIST_FOREACH_SAFE(ska, &sna->sk_adapters, next, tmp) {
		stackmap_unregister_socket(ska);
	}
}

static int
stackmap_bdg_config(struct nm_ifreq *ifr,
			struct netmap_vp_adapter *vpna)
{
	int fd = *(int *)ifr->data;
	struct netmap_adapter *na = &vpna->up;

	return stackmap_register_fd(na, fd);
}

#define STACKMAP_NUM_EXTRA_BUFS	4
//static uint32_t stackmap_extra_bufs[STACKMAP_NUM_EXTRA_BUFS+1];
int
stackmap_reg(struct netmap_adapter *na, int onoff)
{
	struct stackmap_adapter *sna = (struct stackmap_adapter *)na;
	int i, err;

	D("%s (%p) onoff %d suffix: %s",
		na->name, sna, onoff,
		sna->suffix[0] ? sna->suffix : "none");
	err = sna->save_reg(na, onoff);
	if (err)
		return err;
	if (onoff) {
		uint32_t *extra_bufs, n;
		struct netmap_slot *extra_slots;
		struct netmap_bdg_ops ops
			= {NULL, stackmap_bdg_config, stackmap_bdg_dtor};

		/* one extra to store the terminating 0 */
		extra_bufs = nm_os_malloc(sizeof(uint32_t)
		    * STACKMAP_NUM_EXTRA_BUFS + 1);
		if (!extra_bufs) {
			sna->save_reg(na, 0);
			return ENOMEM;
		}
		n = netmap_extra_alloc(na, extra_bufs,
			STACKMAP_NUM_EXTRA_BUFS, 1);
		if (n < STACKMAP_NUM_EXTRA_BUFS)
			D("allocated only %d bufs", n);
		extra_slots = nm_os_malloc(
		    sizeof(struct netmap_slot) * n);
		if (!extra_slots) {
			D("malloc failed for extra slots");
			netmap_extra_free(na, extra_bufs, 1);
			sna->save_reg(na, 0);
			nm_os_free(extra_bufs);
			return ENOMEM;
		}
		for (i = 0; i < n; i++) {
			struct netmap_slot *slot;
		       
			slot = &extra_slots[i];
			slot->buf_idx = extra_bufs[i];
			slot->len = 0;
		}
		sna->extra_bufs = extra_bufs;
		sna->extra_num = n;
		sna->extra_slots = extra_slots;

		/* install config handler */
		netmap_bdg_set_ops(sna->up.na_bdg, &ops);
		//netmap_mem_set_buf_offset(na->nm_mem, STACKMAP_DMA_OFFSET);
		return stackmap_reg_slaves(na);
	}

	/* Build a returning buffer list */
	for (i = 0; i < sna->extra_num; i++) {
		u_int idx = sna->extra_slots[i].buf_idx;
		if (idx >= 2)
			sna->extra_bufs[i] = idx;
	}
	nm_os_free(sna->extra_slots);
	netmap_extra_free(na, sna->extra_bufs, 1);
	nm_os_free(sna->extra_bufs);
	stackmap_unreg_slaves(na);
	return 0;
}


/* allocating skb is postponed until krings are created on register */
static int
stackmap_attach(struct netmap_adapter *arg, struct netmap_adapter **ret,
		const char *suffix)
{
	struct netmap_vp_adapter *vparg = (struct netmap_vp_adapter *)arg;
	struct nm_bridge *b = vparg->na_bdg;
	struct stackmap_adapter *sna;
	struct netmap_vp_adapter *vpna;
	struct netmap_adapter *na;


	sna = nm_os_malloc(sizeof(*sna));
	if (sna == NULL)
		return ENOMEM;
	vpna = &sna->up;
	/* copy everything and replace references from hwna and bridge */
	*vpna = *((struct netmap_vp_adapter *)arg);
	vpna->up.na_vp = vpna;
	netmap_bdg_wlock(b);
	netmap_set_bdg_port(b, vpna->bdg_port, vpna);
	nm_os_free(arg);

	na = &vpna->up;
	sna->save_reg = na->nm_register;
	na->nm_register = stackmap_reg;
	na->nm_txsync = stackmap_txsync;
	na->na_flags |= NAF_BDG_MBUF;
	na->nm_rxsync = stackmap_rxsync;
	strncpy(sna->suffix, suffix, sizeof(sna->suffix));
	NM_LIST_INIT(&sna->sk_adapters);
	netmap_bdg_wunlock(b);
	ND("%s 0x%p (orig 0x%p) mem 0x%p master %d bwrap %d",
	       	na->name, na, arg, na->nm_mem, master, !!nm_is_bwrap(na));

	ND("done (sna %p)", sna);
	*ret = na;
	return 0;
}

int
netmap_get_stackmap_na(struct nmreq *nmr, struct netmap_adapter **ret,
	       	int create)
{
	struct netmap_adapter *na;
	int error;

	*ret = NULL;
	if (strncmp(nmr->nr_name, NM_STACK_NAME, strlen(NM_STACK_NAME)))
		return 0;

	/* XXX always a new, private allocator */
	error = netmap_get_bdg_na(nmr, &na, NULL, create);
	if (error) {
		D("error in get_bdg_na");
		return error;
	}
	/* only master port is extended */
	if (!nm_is_bwrap(na) && na->na_refcount == 1 /* just created */) {
		/* extend the original adapter */
		error = stackmap_attach(na, ret, nmr->nr_suffix);
	} else {
		*ret = na;
	}
	return error;
}
#endif /* WITH_STACK */