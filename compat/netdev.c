// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win compat: dummy net_device and NAPI. A scheduled NAPI instance is polled from one shared work queue
 * thread (PASSIVE_LEVEL; Linux polls in softirq context, which the poll functions already tolerate).
 * state bits: 0 scheduled, 1 disabled, 2 polling.
 */
#include <rtw/linux.h>

#define NAPI_SCHED      0
#define NAPI_DISABLE    1
#define NAPI_POLLING    2

struct napi_os {
	struct work_struct work;
	struct napi_struct *napi;
};

static struct workqueue_struct *napi_wq;
static int napi_users;

struct net_device *alloc_netdev_dummy (int sizeof_priv)
{
	struct net_device *dev = kzalloc(sizeof(*dev) + sizeof_priv, GFP_KERNEL);

	if (!dev)
		return NULL;
	dev->priv = dev + 1;
	INIT_LIST_HEAD(&dev->napi_list);
	strscpy(dev->name, "rtw89-napi", sizeof(dev->name));
	return dev;
}

void free_netdev (struct net_device *dev)
{
	kfree(dev);
}

static void napi_work (struct work_struct *w)
{
	struct napi_os *os = container_of(w, struct napi_os, work);
	struct napi_struct *n = os->napi;
	int done;

	set_bit(NAPI_POLLING, &n->state);
	done = n->poll(n, n->weight);
	clear_bit(NAPI_POLLING, &n->state);
	/* budget used up: the instance stays scheduled and is polled again, unless napi_disable() is waiting
	   for it to be unscheduled (Linux completes it then) */
	if (done >= n->weight && test_bit(NAPI_SCHED, &n->state)) {
		if (test_bit(NAPI_DISABLE, &n->state))
			clear_bit(NAPI_SCHED, &n->state);
		else
			queue_work(napi_wq, &os->work);
	}
}

void netif_napi_add (struct net_device *dev, struct napi_struct *napi, int (*poll)(struct napi_struct *, int))
{
	struct napi_os *os = kzalloc(sizeof(*os), GFP_KERNEL);

	if (!os) {
		rtw_printk(0, "netif_napi_add: out of memory");
		return;
	}
	if (!napi_wq)
		napi_wq = alloc_workqueue("napi", WQ_HIGHPRI, 1);
	napi_users++;
	INIT_WORK(&os->work, napi_work);
	os->napi = napi;
	napi->os = os;
	napi->dev = dev;
	napi->poll = poll;
	napi->weight = NAPI_POLL_WEIGHT;
	napi->state = BIT(NAPI_SCHED) | BIT(NAPI_DISABLE);     /* like Linux: disabled until napi_enable() */
	list_add_tail(&napi->dev_list, &dev->napi_list);
}

void netif_napi_del (struct napi_struct *napi)
{
	struct napi_os *os = napi->os;

	if (!os)
		return;
	cancel_work_sync(&os->work);
	list_del(&napi->dev_list);
	kfree(os);
	napi->os = NULL;
	if (--napi_users == 0) {
		destroy_workqueue(napi_wq);
		napi_wq = NULL;
	}
}

bool napi_schedule_prep (struct napi_struct *napi)
{
	if (test_bit(NAPI_DISABLE, &napi->state))
		return false;
	return !test_and_set_bit(NAPI_SCHED, &napi->state);
}

void __napi_schedule (struct napi_struct *napi)
{
	struct napi_os *os = napi->os;

	if (os)
		queue_work(napi_wq, &os->work);
}

bool napi_complete_done (struct napi_struct *napi, int work_done)
{
	(void)work_done;
	clear_bit(NAPI_SCHED, &napi->state);
	return true;
}

void napi_synchronize (struct napi_struct *napi)
{
	while (test_bit(NAPI_POLLING, &napi->state))
		rtwk_usleep(100);
}

void napi_disable (struct napi_struct *napi)
{
	set_bit(NAPI_DISABLE, &napi->state);
	while (test_and_set_bit(NAPI_SCHED, &napi->state))
		rtwk_usleep(100);
	napi_synchronize(napi);
}

void napi_enable (struct napi_struct *napi)
{
	clear_bit(NAPI_SCHED, &napi->state);
	clear_bit(NAPI_DISABLE, &napi->state);
}

int netif_receive_skb (struct sk_buff *skb)
{
	kfree_skb(skb);
	return 0;
}
