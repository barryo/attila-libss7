/*
 * libss7: An implementation of Signalling System 7
 *
 * Written by Matthew Fredrickson <creslin@digium.com>
 *
 * scheduling routines taken from libpri by Mark Spencer <markster@digium.com>
 *
 * Copyright (C) 2006-2008, Digium, Inc
 * All Rights Reserved.
 */

/*
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2 as published by the
 * Free Software Foundation. See the LICENSE file included with
 * this program for more details.
 *
 * In addition, when this program is distributed with Asterisk in
 * any form that would qualify as a 'combined work' or as a
 * 'derivative work' (but not mere aggregation), you can redistribute
 * and/or modify the combination under the terms of the license
 * provided with that copy of Asterisk, instead of the license
 * terms granted here.
 */

#include "ss7_internal.h"
#include "mtp3.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include "mtp2.h"

#define mtp_error ss7_error
#define mtp_message ss7_message

int len_buf(struct ss7_msg *buf)
{
	int res = 0;
	struct ss7_msg *cur = buf;

	while (cur) {
		res++;
		cur = cur->next;
	}
	return res;
}
		
static inline char * linkstate2str(int linkstate)
{
	char *statestr = NULL;

	switch (linkstate) {
		case MTP_IDLE:
			statestr = "IDLE";
			break;
		case MTP_NOTALIGNED:
			statestr = "NOTALIGNED";
			break;
		case MTP_ALIGNED:
			statestr = "ALIGNED";
			break;
		case MTP_PROVING:
			statestr = "PROVING";
			break;
		case MTP_ALIGNEDREADY:
			statestr = "ALIGNEDREADY";
			break;
		case MTP_INSERVICE:
			statestr = "INSERVICE";
			break;
		case MTP_ALARM:
			statestr = "ALARM";
			break;
		default:
			statestr = "UNKNOWN";
	}

	return statestr;
}

char *linkstate2strext(int linkstate)
{
	return linkstate2str(linkstate);
}

static inline void init_mtp2_header(struct mtp2 *link, struct mtp_su_head *h, int new, int nack)
{
	if (new) {
		link->curfsn += 1;
		link->flags |= MTP2_FLAG_WRITE;
	}

	h->fib = link->curfib;
	h->fsn = link->curfsn;
	
	if (nack) {
		link->curbib = !link->curbib;
		link->flags |= MTP2_FLAG_WRITE;
	}

	h->bib = link->curbib;
	h->bsn = link->lastfsnacked;
}

static inline int lssu_type(struct mtp_su_head *h)
{
	return h->data[0];
}

void flush_bufs(struct mtp2 *link)
{
	struct ss7_msg *list, *cur;

	list = link->tx_buf;

	link->tx_buf = NULL;

	while (list) {
		cur = list;
		list = list->next;
		free(cur);
	}

	list = link->tx_q;

	link->tx_q = NULL;

	while (list) {
		cur = list;
		list = list->next;
		free(cur);
	}

	link->retransmit_pos = NULL;
}

static void reset_mtp(struct mtp2 *link)
{
	link->curfsn = 127;
	link->curfib = 1;
	link->curbib = 1;
#if 0
	ss7_message(link->master, "Lastfsn: %i txbuflen: %i SLC: %i ADJPC: %i\n", link->lastfsnacked, len_buf(link->tx_buf), link->slc, link->dpc);
#endif
	link->lastfsnacked = 127;
	link->retransmissioncount = 0;
	link->flags |= MTP2_FLAG_WRITE;

	flush_bufs(link);
}


static void mtp2_request_retransmission(struct mtp2 *link)
{
	link->retransmissioncount++;
	link->curbib = !link->curbib;
	link->flags |= MTP2_FLAG_WRITE;
}

static int mtp2_queue_su(struct mtp2 *link, struct ss7_msg *m)
{
	struct ss7_msg *cur;
	
	if (!link->tx_q) {
		link->tx_q = m;
		m->next = NULL;
		return 0;
	}

	for (cur = link->tx_q; cur->next; cur = cur->next);
	
	cur->next = m;
	m->next = NULL;
	
	return 0;
}

static void make_lssu(struct mtp2 *link, unsigned char *buf, unsigned int *size, int lssu_status)
{
	struct mtp_su_head *head;

	*size = LSSU_SIZE;

	memset(buf, 0, LSSU_SIZE);

	head = (struct mtp_su_head *)buf;
	head->li = 1;
	switch (lssu_status) {
		case LSSU_SIOS:
		case LSSU_SIO:
			reset_mtp(link);
		case LSSU_SIN:
		case LSSU_SIE:
		case LSSU_SIPO:
		case LSSU_SIB:
			head->bib = link->curbib;
			head->bsn = link->lastfsnacked;
			head->fib = link->curfib;
			head->fsn = link->curfsn;
			break;
	}

	head->data[0] = lssu_status;
}

static void make_fisu(struct mtp2 *link, unsigned char *buf, unsigned int *size, int nack)
{
	struct mtp_su_head *h;

	*size = FISU_SIZE;

	h = (struct mtp_su_head *)buf;

	memset(buf, 0, *size);

	init_mtp2_header(link, h, 0, nack);

	h->li = 0;
}

static void add_txbuf(struct mtp2 *link, struct ss7_msg *m)
{
	m->next = link->tx_buf;
	link->tx_buf = m;
#if 0
	mtp_message(link->master, "Txbuf contains %d items\n", len_buf(link->tx_buf));
#endif
}

static void update_retransmit_pos(struct mtp2 *link)
{
	struct ss7_msg *cur, *prev = NULL;
	/* Our txbuf is in reversed order from the order we need to retransmit in */

	cur = link->tx_buf;

	while (cur) {
		if (cur == link->retransmit_pos)
			break;
		prev = cur;
		cur = cur->next;
	}

	link->retransmit_pos = prev;
	
}

static void mtp2_retransmit(struct mtp2 *link)
{
	struct ss7_msg *m;

	link->flags |= MTP2_FLAG_WRITE;
	/* Have to invert the current fib */
	link->curfib = !link->curfib;

	m = link->tx_buf;
	if (!m) {
		ss7_error(link->master, "Huh!? Asked to retransmit but we don't have anything in the tx buffer\n");
		return;
	}

	while (m->next)
		m = m->next;

	link->retransmit_pos = m;
}

static void t7_expiry(void *data)
{
	struct mtp2 *link = data;
	ss7_error(link->master, "T7 expired on link SLC: %i ADJPC: %i\n", link->slc, link->dpc);
	link->t7 = -1;
	mtp2_setstate(link, MTP_IDLE);
}

int mtp2_transmit(struct mtp2 *link)
{
	int res = 0;
	unsigned char *h;
	unsigned char buf[64];
	unsigned int size;
	struct ss7_msg *m = NULL;
	int retransmit = 0;

	if (link->send_sios) {
		link->send_sios = 0;
		make_lssu(link, buf, &size, LSSU_SIOS);
		h = buf;

		res = write(link->fd, h, size);

		if (res > 0) {
			mtp2_dump(link, '>', h, size - 2);
		}
	}

	if (link->retransmit_pos) {
		struct mtp_su_head *h1;
		m = link->retransmit_pos;
		retransmit = 1;

		if (!m) {
			ss7_error(link->master, "Huh, requested to retransmit, but nothing in retransmit buffer?!!\n");
			return -1;
		}

		h = m->buf;
		size = m->size;

		h1 = (struct mtp_su_head *)h;
		/* Update the FIB and BSN since they aren't the same */
		h1->fib = link->curfib;
		h1->bsn = link->lastfsnacked;

	} else {
		if (link->tx_q)
			m = link->tx_q;
	
		if (m) {
			h = m->buf;
			init_mtp2_header(link, (struct mtp_su_head *) h, 1, 0); /* in changeover we may manipulate the buffers!!! */
			size = m->size;
		} else {
			size = sizeof(buf);
			if (link->autotxsutype == FISU)
				make_fisu(link, buf, &size, 0);
			else
				make_lssu(link, buf, &size, link->autotxsutype);
			h = buf;
		}
	}

	res = write(link->fd, h, size);  /* Add 2 for FCS */

	if (res > 0) {
		mtp2_dump(link, '>', h, size - 2);
		if (retransmit) {
			/* Update our retransmit positon since it transmitted */
			update_retransmit_pos(link);
		} else {
			if (m) {
				/* Advance to next MSU to be transmitted */
				link->tx_q = m->next;
				/* Add it to the tx'd message queue (MSUs that haven't been acknowledged) */
				add_txbuf(link, m);
				if (link->t7 == -1)
					link->t7 = ss7_schedule_event(link->master, link->timers.t7, t7_expiry, link);
			}
		}

		if (h == buf) { /* We just sent a non MSU */
			link->flags &= ~MTP2_FLAG_WRITE;
		}
	}

	return res;
}

int mtp2_msu(struct mtp2 *link, struct ss7_msg *m)
{
	int len = m->size - MTP2_SIZE;
	struct mtp_su_head *h = (struct mtp_su_head *) m->buf;

	link->flags |= MTP2_FLAG_WRITE;

	/* init_mtp2_header(link, h, 1, 0); */

	if (len > MTP2_LI_MAX)
		h->li = MTP2_LI_MAX;
	else
		h->li = len;

	m->size += 2; /* For CRC */
	mtp2_queue_su(link, m);
	/* Just in case */
	m->next = NULL;

	return 0;
}

static int mtp2_lssu(struct mtp2 *link, int lssu_status)
{
	link->flags |= MTP2_FLAG_WRITE;
	link->autotxsutype = lssu_status;
	return 0;
}

static int mtp2_fisu(struct mtp2 *link, int nack)
{
	link->flags |= MTP2_FLAG_WRITE;
	link->autotxsutype = FISU;
	return 0;
}

void update_txbuf(struct mtp2 *link, struct ss7_msg **buf, unsigned char upto)
{
	struct mtp_su_head *h;
	struct ss7_msg *prev = NULL, *cur;
	struct ss7_msg *frlist = NULL;
	/* Make a list, frlist that will be the SUs to free */

	/* Empty list */
	if (!*buf) {
		return;
	}

	cur = *buf;

	while (cur) {
		h = (struct mtp_su_head *)cur->buf;
		if (h->fsn == upto) {
			frlist = cur;
			if (!prev) /* Head of list */
				*buf = NULL;
			else
				prev->next = NULL;
			frlist = cur;
			break;
		}
		prev = cur;
		cur = cur->next;
	}

	if (link && frlist && link->t7 > -1) {
		ss7_schedule_del(link->master, &link->t7);
		if (link->tx_buf)
			link->t7 = ss7_schedule_event(link->master, link->timers.t7, &t7_expiry, link);
	}
	
	while (frlist) {
		cur = frlist;
		frlist = frlist->next;
		free(cur);
	}

	return;
}

static int fisu_rx(struct mtp2 *link, struct mtp_su_head *h, int len)
{
	if (link->lastsurxd == FISU)
		return 0;
	else
		link->lastsurxd = FISU;

	switch (link->state) {
		case MTP_PROVING:
			return mtp2_setstate(link, MTP_ALIGNEDREADY);
			/* Just in case our timers are a little off */
		case MTP_ALIGNEDREADY:
			mtp2_setstate(link, MTP_INSERVICE);
		case MTP_INSERVICE:
			if (h->fsn != link->lastfsnacked) {
				mtp_message(link->master, "Received out of sequence FISU w/ fsn of %d, lastfsnacked = %d, requesting retransmission\n", h->fsn, link->lastfsnacked);
				mtp2_request_retransmission(link);
			}
			break;
		default:
			mtp_message(link->master, "Huh?! Got FISU in link state %d\n", link->state);
			return -1;
	}
	
	return 0;
}

static void t1_expiry(void *data)
{
	struct mtp2 *link = data;

	mtp2_setstate(link, MTP_IDLE);

	return;
}

static void t2_expiry(void * data)
{
	struct mtp2 *link = data;

	//mtp2_setstate(link, MTP_IDLE);
	link->state = MTP_NOTALIGNED;
	link->t2 = ss7_schedule_event(link->master, link->timers.t2, t2_expiry, link);
	mtp2_lssu(link, LSSU_SIO);
	link->send_sios = 1;
	return;
}

static void t3_expiry(void * data)
{
	struct mtp2 *link = data;
	
	mtp2_setstate(link, MTP_IDLE);

	return;
}

static void t4_expiry(void * data)
{
	struct mtp2 *link = data;

	if (link->master->debug & SS7_DEBUG_MTP2)
		ss7_message(link->master, "T4 expired!\n");

	mtp2_setstate(link, MTP_ALIGNEDREADY);

	return;
}

static int to_idle(struct mtp2 *link)
{
	link->state = MTP_IDLE;
	if (mtp2_lssu(link, LSSU_SIOS)) {
		mtp_error(link->master, "Could not transmit LSSU\n");
		return -1;
	}

	mtp2_setstate(link, MTP_NOTALIGNED);

	return 0;
}

int mtp2_setstate(struct mtp2 *link, int newstate)
{
	ss7_event *e;

	if (link->master->debug & SS7_DEBUG_MTP2)
		mtp_message(link->master, "Link state change: %s -> %s\n", linkstate2str(link->state), linkstate2str(newstate));

	switch (link->state) {
		case MTP_ALARM:
			return 0;
		case MTP_IDLE:
			link->t2 = ss7_schedule_event(link->master, link->timers.t2, t2_expiry, link);
			if (mtp2_lssu(link, LSSU_SIO)) {
				mtp_error(link->master, "Unable to transmit initial LSSU\n");
				return -1;
			}
			link->state = MTP_NOTALIGNED;
			return 0;
		case MTP_NOTALIGNED:
			ss7_schedule_del(link->master, &link->t2);
			switch (newstate) {
				case MTP_IDLE:
					return to_idle(link);
				case MTP_ALIGNED:
				case MTP_PROVING:
					if (newstate == MTP_ALIGNED)
						link->t3 = ss7_schedule_event(link->master, link->timers.t3, t3_expiry, link);
					else
						link->t4 = ss7_schedule_event(link->master, link->provingperiod, t4_expiry, link);
					if (link->emergency) {
						if (mtp2_lssu(link, LSSU_SIE)) {
							mtp_error(link->master, "Couldn't tx LSSU_SIE\n");
							return -1;
						}
					} else {
						if (mtp2_lssu(link, LSSU_SIN)) {
							mtp_error(link->master, "Couldn't tx LSSU_SIE\n");
							return -1;
						}
					}
					break;
			}
			link->state = newstate;
			return 0;
		case MTP_ALIGNED:
			ss7_schedule_del(link->master, &link->t3);

			switch (newstate) {
				case MTP_IDLE:
					return to_idle(link);
				case MTP_PROVING:
					link->t4 = ss7_schedule_event(link->master, link->provingperiod, t4_expiry, link);
			}
			link->state = newstate;
			return 0;
		case MTP_PROVING:
			ss7_schedule_del(link->master, &link->t4);

			switch (newstate) {
				case MTP_IDLE:
					return to_idle(link);
				case MTP_PROVING:
					link->t4 = ss7_schedule_event(link->master, link->provingperiod, t4_expiry, link);
					break;
				case MTP_ALIGNED:
					if (link->emergency) {
						if (mtp2_lssu(link, LSSU_SIE)) {
							mtp_error(link->master, "Could not transmit LSSU\n");
							return -1;
						}
					} else {
						if (mtp2_lssu(link, LSSU_SIN)) {
							mtp_error(link->master, "Could not transmit LSSU\n");
							return -1;
						}
					}
					break;
				case MTP_ALIGNEDREADY:
					link->t1 = ss7_schedule_event(link->master, link->timers.t1, t1_expiry, link);
					if (mtp2_fisu(link, 0)) {
						mtp_error(link->master, "Could not transmit FISU\n");
						return -1;
					}
					break;
			}
			link->state = newstate;
			return 0;
		case MTP_ALIGNEDREADY:
			ss7_schedule_del(link->master, &link->t1);
			/* Our timer expired, it should be cleaned up already */
			switch (newstate) {
				case MTP_IDLE:
					return to_idle(link);
				case MTP_ALIGNEDREADY:
					link->t1 = ss7_schedule_event(link->master, link->timers.t1, t1_expiry, link);
					if (mtp2_fisu(link, 0)) {
						mtp_error(link->master, "Could not transmit FISU\n");
						return -1;
					}
					break;
				case MTP_INSERVICE:
					ss7_schedule_del(link->master, &link->t1);
					e = ss7_next_empty_event(link->master);
					if (!e) {
						mtp_error(link->master, "Could not queue event\n");
						return -1;
					}
					e->link.e = MTP2_LINK_UP;
					e->link.link = link;
					break;
				default:
					mtp_error(link->master, "Don't know how to handle state change from %d to %d\n", link->state, newstate);
					break;
			}
			link->state = newstate;
			return 0;
		case MTP_INSERVICE:
			if (newstate != MTP_INSERVICE) {
				e = ss7_next_empty_event(link->master);
				if (!e) {
					mtp_error(link->master, "Could not queue event\n");
					return -1;
				}
				e->link.e = MTP2_LINK_DOWN;
				e->link.link = link;
				return to_idle(link);
			}
			break;
	}
	return 0;
}

static int lssu_rx(struct mtp2 *link, struct mtp_su_head *h, int len)
{
	unsigned char lssutype = lssu_type(h);

	if (len > (LSSU_SIZE + 2))  /* FCS is two bytes */
		mtp_error(link->master, "Received LSSU with length %d longer than expected\n", len);

	if (link->lastsurxd == lssutype)
		return 0;
	else
		link->lastsurxd = lssutype;

	if (lssutype == LSSU_SIE)
		link->emergency = 1;

	if (lssutype == LSSU_SIOS && link->state == MTP_NOTALIGNED) {
		link->send_sios = 1;
	}

	switch (link->state) {
		case MTP_IDLE:
		case MTP_NOTALIGNED:
			if ((lssutype != LSSU_SIE) && (lssutype != LSSU_SIN) && (lssutype != LSSU_SIO))
				return mtp2_setstate(link, MTP_NOTALIGNED);

			if ((link->emergency) || (lssutype == LSSU_SIE))
				link->provingperiod = link->timers.t4e;
			else
				link->provingperiod = link->timers.t4;

			if ((lssutype == LSSU_SIE) || (lssutype == LSSU_SIN))
				return mtp2_setstate(link, MTP_PROVING);
			else
				return mtp2_setstate(link, MTP_ALIGNED);
		case MTP_ALIGNED:
			if (lssutype == LSSU_SIOS)
				return mtp2_setstate(link, MTP_IDLE);

			if ((link->emergency) || (lssutype == LSSU_SIE))
				link->provingperiod = link->timers.t4e;
			else
				link->provingperiod = link->timers.t4;

			if ((link->provingperiod == link->timers.t4) && ((link->emergency) || (lssutype == LSSU_SIE)))
				link->provingperiod = link->timers.t4e;

			return mtp2_setstate(link, MTP_PROVING);
		case MTP_PROVING:
			if (lssutype == LSSU_SIOS)
				return mtp2_setstate(link, MTP_IDLE);

			if (lssutype == LSSU_SIO)
				return mtp2_setstate(link, MTP_ALIGNED);
			
			mtp_message(link->master, "Don't handle any other conditions in state %d\n", link->state);
			break;
		case MTP_ALIGNEDREADY:
		case MTP_INSERVICE:
			if ((lssutype != LSSU_SIOS) && (lssutype != LSSU_SIO))
				mtp_message(link->master, "Got LSSU of type %d while link is in state %d.  Re-Aligning\n", lssutype, link->state);
			return mtp2_setstate(link, MTP_IDLE);

	}

	return 0;
}

static int msu_rx(struct mtp2 *link, struct mtp_su_head *h, int len)
{
	int res = 0;

	switch (link->state) {
		case MTP_ALIGNEDREADY:
			mtp2_setstate(link, MTP_INSERVICE);
			break;
		case MTP_INSERVICE:
			break;
		default:
			mtp_error(link->master, "Received MSU in invalid state %d\n", link->state);
			return -1;
	}

	/* If we're still waiting for our retranmission acknownledgement, we'll just ignore subsequent MSUs until it starts */
	if (h->fib != link->curbib) {
		mtp_message(link->master, "MSU received, though still waiting for retransmission start.  Dropping.\n");
		return 0;
	}

	if (h->fsn == link->lastfsnacked) {
		/* Discard */
		mtp_message(link->master, "Received double MSU, dropping\n");
		return 0;
	}

	if (h->fsn != ((link->lastfsnacked+1) % 128)) {
		mtp_message(link->master, "Received out of sequence MSU w/ fsn of %d, lastfsnacked = %d, requesting retransmission\n", h->fsn, link->lastfsnacked);
		mtp2_request_retransmission(link);
		return 0;
	}

	/* Ok, it's a valid MSU now and we can accept it */
	link->lastfsnacked = h->fsn;
	/* Set write flag since we need to update the FISUs with our new BSN */
	link->flags |= MTP2_FLAG_WRITE;
	/* The big function */
	res = mtp3_receive(link->master, link, h->data, len - MTP2_SU_HEAD_SIZE);

	return res;
}

int mtp2_start(struct mtp2 *link, int emergency)
{
	reset_mtp(link);
	link->emergency = emergency;
	if (link->state == MTP_IDLE)
		return mtp2_setstate(link, MTP_NOTALIGNED);
	else
		return 0;
}

int mtp2_stop(struct mtp2 *link)
{
	return mtp2_setstate(link, MTP_IDLE);
}

int mtp2_alarm(struct mtp2 *link)
{
	link->state = MTP_ALARM;
	return 0;
}

int mtp2_noalarm(struct mtp2 *link)
{
	link->state = MTP_IDLE;
	return 0;
}

struct mtp2 * mtp2_new(int fd, unsigned int switchtype)
{
	struct mtp2 * new = calloc(1, sizeof(struct mtp2));
	int x;
	
	if (!new)
		return NULL;

	reset_mtp(new);

	new->fd = fd;
	new->autotxsutype = LSSU_SIOS;
	new->send_sios = 0;
	new->lastsurxd = -1;
	new->lastsutxd = -1;

	if (switchtype == SS7_ITU) {
		new->timers.t1 = ITU_TIMER_T1;
		new->timers.t2 = ITU_TIMER_T2;
		new->timers.t3 = ITU_TIMER_T3;
		new->timers.t4 = ITU_TIMER_T4_NORMAL;
		new->timers.t4e = ITU_TIMER_T4_EMERGENCY;
		new->timers.t7 = ITU_TIMER_T7;
	} else if (switchtype == SS7_ANSI) {
		new->timers.t1 = ANSI_TIMER_T1;
		new->timers.t2 = ANSI_TIMER_T2;
		new->timers.t3 = ANSI_TIMER_T3;
		new->timers.t4 = ANSI_TIMER_T4_NORMAL;
		new->timers.t4e = ANSI_TIMER_T4_EMERGENCY;
		new->timers.t7 = ANSI_TIMER_T7;
	}
	
	for (x = 0; x < MTP3_MAX_TIMERS; x++)
		new->mtp3_timer[x] = -1;
	
	return new;
}


void mtp2_dump(struct mtp2 *link, char prefix, unsigned char *buf, int len)
{
	struct mtp_su_head *h = (struct mtp_su_head *)buf;
	unsigned char mtype;
	char *mtypech = NULL;

	if (!(link->master->debug & SS7_DEBUG_MTP2))
		return;

	switch (h->li) {
		case 0:
			mtype = 0;
			break;
		case 1:
		case 2:
			mtype = 1;
			break;
		default:
			mtype = 2;
			break;
	}


	switch (mtype) {
		case 0:
			if (prefix == '<' && link->lastsurxd == FISU)
				return;
			if (prefix == '>' && link->lastsutxd == FISU)
				return;
			else
				link->lastsutxd = FISU;
			ss7_dump_msg(link->master, buf, len);
			ss7_message(link->master, "FSN: %d FIB %d\n", h->fsn, h->fib);
			ss7_message(link->master, "BSN: %d BIB %d\n", h->bsn, h->bib);

			ss7_message(link->master, "%c[%d] FISU\n", prefix, link->slc);
			break; 
		case 1:
			//if (prefix == '<' && link->lastsurxd == h->data[0])
			//	return;
			//if (prefix == '>' && link->lastsutxd == h->data[0])
			//	return;
			//else
				link->lastsutxd = h->data[0];
			switch (h->data[0]) {
				case LSSU_SIOS:
					mtypech = "SIOS";
					break;
				case LSSU_SIO:
					mtypech = "SIO";
					break;
				case LSSU_SIN:
					mtypech = "SIN";
					break;
				case LSSU_SIE:
					mtypech = "SIE";
					break;
				case LSSU_SIPO:
					mtypech = "SIPO";
					break;
				case LSSU_SIB:
					mtypech = "SIB";
					break;
			}
			ss7_dump_msg(link->master, buf, len);
			ss7_message(link->master, "FSN: %d FIB %d\n", h->fsn, h->fib);
			ss7_message(link->master, "BSN: %d BIB %d\n", h->bsn, h->bib);
			ss7_message(link->master, "%c[%d] LSSU %s\n", prefix, link->slc, mtypech);
			break;
		case 2:
			ss7_dump_msg(link->master, buf, len);
			ss7_message(link->master, "FSN: %d FIB %d\n", h->fsn, h->fib);
			ss7_message(link->master, "BSN: %d BIB %d\n", h->bsn, h->bib);
			ss7_message(link->master, "%c[%d] MSU\n", prefix, link->slc);
			ss7_dump_buf(link->master, 0, buf, 3);
			mtp3_dump(link->master, link, h->data, len - MTP2_SU_HEAD_SIZE);
			break;
	}

	ss7_message(link->master, "\n");
}

/* returns an event */
int mtp2_receive(struct mtp2 *link, unsigned char *buf, int len)
{
	struct mtp_su_head *h = (struct mtp_su_head *)buf;
	len -= 2; /* Strip the CRC off */

	if (len < MTP2_SIZE) {
		ss7_message(link->master, "Got message smaller than the minimum SS7 SU length.  Dropping\n");
		return 0;
	}
	
	mtp2_dump(link, '<', buf, len);

	update_txbuf(link, &link->tx_buf, h->bsn);

	/* Check for retransmission request */
	if ((link->state == MTP_INSERVICE) &&  (h->bib != link->curfib)) {
		/* Negative ack */
		ss7_message(link->master, "Got retransmission request sequence numbers greater than %d. Retransmitting %d message(s).\n", h->bsn, len_buf(link->tx_buf));
		mtp2_retransmit(link);
	}

	switch (h->li) {
		case 0:
			/* FISU */
			return fisu_rx(link, h, len);
		case 1:
		case 2:
			/* LSSU */
			return lssu_rx(link, h, len);
		default:
			/* MSU */
			return msu_rx(link, h, len);
	}

	return 0;
}
