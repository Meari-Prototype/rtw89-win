// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win compat: RCU (rtw/sched.h). Readers increment one global counter; a writer unlinks, then
 * synchronize_rcu() waits until the counter has dropped to zero, which every reader that started before the call
 * has to pass. Read-side sections are short and never sleep (rtw89's NAPI thread, the mac layer's iterators and
 * its data path at DISPATCH_LEVEL), so the counter reaches zero quickly even under traffic.
 * call_rcu()/kfree_rcu() queue the callback; a work item on the system queue waits for the readers and runs the
 * whole batch.
 */
#include <rtw/linux.h>

#define RCU_SLOW_WARN_MS                1000

atomic_t rtw_rcu_readers;

static DEFINE_SPINLOCK(rcu_cb_lock);
static struct rcu_head *rcu_cb_list;    /* LIFO; order does not matter for frees */
static struct work_struct rcu_cb_work;
static bool rcu_up;

void synchronize_rcu (void)
{
	u64 start;
	bool warned = false;

	if (rtwk_current_irql() > 0) {
		/* cannot wait here; rtw89 only synchronizes from process context, so this would be a mac layer bug */
		WARN_ONCE(1, "rcu: synchronize_rcu at IRQL %d", rtwk_current_irql());
		return;
	}
	smp_mb();
	rtwk_timer_sync();              /* DPCs (timer callbacks) that might still look at the object */
	start = rtwk_time_ns();
	while (atomic_read(&rtw_rcu_readers)) {
		rtwk_usleep(20);
		if (!warned && rtwk_time_ns() - start > RCU_SLOW_WARN_MS * NSEC_PER_MSEC) {
			warned = true;
			pr_warn("rcu: readers still active after %u ms", RCU_SLOW_WARN_MS);
		}
	}
	smp_mb();
}

static void rcu_cb_run (struct work_struct *w)
{
	struct rcu_head *list, *next;

	(void)w;
	spin_lock_bh(&rcu_cb_lock);
	list = rcu_cb_list;
	rcu_cb_list = NULL;
	spin_unlock_bh(&rcu_cb_lock);
	if (!list)
		return;
	synchronize_rcu();
	for (; list; list = next) {
		next = list->next;
		if (list->func)
			list->func(list);
		else
			kfree(list->obj);
	}
}

static void rcu_cb_add (struct rcu_head *head)
{
	spin_lock_bh(&rcu_cb_lock);
	head->next = rcu_cb_list;
	rcu_cb_list = head;
	spin_unlock_bh(&rcu_cb_lock);
	if (READ_ONCE(rcu_up))
		queue_work(system_wq, &rcu_cb_work);
	else if (rtwk_current_irql() == 0)
		rcu_cb_run(NULL);       /* no work queue (yet or any more): wait right here */
	else
		pr_err("rcu: callback queued at DISPATCH_LEVEL without a work queue; it runs at the next exit");
}

void call_rcu (struct rcu_head *head, void (*fn)(struct rcu_head *))
{
	head->func = fn;
	head->obj = NULL;
	rcu_cb_add(head);
}

void rtw_kfree_rcu (struct rcu_head *head, void *obj)
{
	head->func = NULL;
	head->obj = obj;
	rcu_cb_add(head);
}

void rcu_barrier (void)
{
	if (READ_ONCE(rcu_up))
		flush_work(&rcu_cb_work);
	rcu_cb_run(NULL);
}

/* after rtw_workqueue_init() */
int rtw_rcu_init (void)
{
	INIT_WORK(&rcu_cb_work, rcu_cb_run);
	WRITE_ONCE(rcu_up, true);
	return 0;
}

/* before rtw_workqueue_exit(): nothing may stay queued */
void rtw_rcu_exit (void)
{
	if (!READ_ONCE(rcu_up))
		return;
	WRITE_ONCE(rcu_up, false);
	cancel_work_sync(&rcu_cb_work);
	rcu_cb_run(NULL);
}
