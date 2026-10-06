// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win compat: timer_list (KTIMER + DPC through rtwk_timer_*) and work queues. Each work queue is one
 * PASSIVE_LEVEL kernel thread running its items in order, so every queue is "ordered" (max_active = 1); rtw89
 * does not depend on parallel execution. All queue state is under one spin lock.
 */
#include <rtw/linux.h>

/* ---- timers ---- */
static void timer_trampoline (void *ctx)
{
	struct timer_list *t = ctx;

	WRITE_ONCE(t->pending, false);
	t->function(t);
}

void timer_setup (struct timer_list *t, void (*fn)(struct timer_list *), unsigned int flags)
{
	t->function = fn;
	t->flags = flags;
	t->pending = false;
	rtwk_timer_init(t->os, timer_trampoline, t);
}

int mod_timer (struct timer_list *t, unsigned long expires)
{
	long delta = (long)(expires - jiffies);
	bool was = READ_ONCE(t->pending);

	t->expires = expires;
	WRITE_ONCE(t->pending, true);
	rtwk_timer_arm(t->os, delta > 0 ? (u64)delta * 1000 : 0);
	return was;
}

int timer_delete (struct timer_list *t)
{
	int was = rtwk_timer_cancel(t->os);

	WRITE_ONCE(t->pending, false);
	return was;
}

int timer_delete_sync (struct timer_list *t)
{
	int was = timer_delete(t);

	rtwk_timer_sync();              /* a callback that already started has finished... */
	was |= timer_delete(t);         /* ...and if it re-armed the timer, that is cancelled too */
	return was;
}

/* ---- work queues ---- */
struct workqueue_struct {
	char name[32];
	struct list_head list;
	u64 event[RTWK_EVENT_SIZE / 8];
	void *thread;
	bool stop;
	struct work_struct *current_work;
};

static spinlock_t wq_lock;
struct workqueue_struct *system_wq;
struct workqueue_struct *system_long_wq;
struct workqueue_struct *system_unbound_wq;

static void worker (void *ctx)
{
	struct workqueue_struct *wq = ctx;

	for (;;) {
		struct work_struct *w = NULL;

		spin_lock(&wq_lock);
		if (!list_empty(&wq->list)) {
			w = list_first_entry(&wq->list, struct work_struct, entry);
			list_del_init(&w->entry);
			w->wq = NULL;
			w->running_on = wq;
			wq->current_work = w;
		}
		spin_unlock(&wq_lock);

		if (w) {
			w->func(w);
			spin_lock(&wq_lock);
			/* the item may have been freed or re-queued by its own function; only clear what is ours */
			if (wq->current_work == w) {
				wq->current_work = NULL;
				if (w->running_on == wq)
					w->running_on = NULL;
			}
			spin_unlock(&wq_lock);
			continue;
		}
		if (READ_ONCE(wq->stop))
			break;
		rtwk_event_wait(wq->event, -1);
	}
}

struct workqueue_struct *alloc_workqueue (const char *fmt, unsigned int flags, int max_active, ...)
{
	struct workqueue_struct *wq = kzalloc(sizeof(*wq), GFP_KERNEL);
	va_list ap;

	(void)flags;
	(void)max_active;
	if (!wq)
		return NULL;
	va_start(ap, max_active);
	vscnprintf(wq->name, sizeof(wq->name), fmt, ap);
	va_end(ap);
	INIT_LIST_HEAD(&wq->list);
	rtwk_event_init(wq->event, 0);
	wq->thread = rtwk_thread_start(worker, wq);
	if (!wq->thread) {
		kfree(wq);
		return NULL;
	}
	return wq;
}

void flush_workqueue (struct workqueue_struct *wq)
{
	for (;;) {
		bool busy;

		spin_lock(&wq_lock);
		busy = !list_empty(&wq->list) || wq->current_work;
		spin_unlock(&wq_lock);
		if (!busy)
			return;
		rtwk_usleep(200);
	}
}

void destroy_workqueue (struct workqueue_struct *wq)
{
	if (!wq)
		return;
	flush_workqueue(wq);
	WRITE_ONCE(wq->stop, true);
	rtwk_event_set(wq->event);
	rtwk_thread_join(wq->thread);
	kfree(wq);
}

void __init_work (struct work_struct *w, work_func_t fn)
{
	INIT_LIST_HEAD(&w->entry);
	w->func = fn;
	w->wq = NULL;
	w->running_on = NULL;
	w->data = 0;
}

bool queue_work (struct workqueue_struct *wq, struct work_struct *w)
{
	bool queued = false;

	spin_lock(&wq_lock);
	if (!w->wq) {
		w->wq = wq;
		list_add_tail(&w->entry, &wq->list);
		queued = true;
	}
	spin_unlock(&wq_lock);
	if (queued)
		rtwk_event_set(wq->event);
	return queued;
}

bool cancel_work (struct work_struct *w)
{
	bool was = false;

	spin_lock(&wq_lock);
	if (w->wq) {
		list_del_init(&w->entry);
		w->wq = NULL;
		was = true;
	}
	spin_unlock(&wq_lock);
	return was;
}

static bool wait_idle (struct work_struct *w, bool also_pending)
{
	bool waited = false;

	for (;;) {
		bool busy;

		spin_lock(&wq_lock);
		busy = w->running_on != NULL || (also_pending && w->wq != NULL);
		spin_unlock(&wq_lock);
		if (!busy)
			return waited;
		waited = true;
		rtwk_usleep(100);
	}
}

bool cancel_work_sync (struct work_struct *w)
{
	bool was = cancel_work(w);

	return wait_idle(w, false) || was;
}

bool flush_work (struct work_struct *w)
{
	return wait_idle(w, true);
}

bool work_pending (const struct work_struct *w)
{
	return READ_ONCE(w->wq) != NULL;
}

/* ---- delayed work ---- */
static void delayed_work_timer_fn (struct timer_list *t)
{
	struct delayed_work *dw = from_timer(dw, t, timer);

	queue_work(dw->wq ? dw->wq : system_wq, &dw->work);
}

void __init_delayed_work (struct delayed_work *dw, work_func_t fn)
{
	__init_work(&dw->work, fn);
	timer_setup(&dw->timer, delayed_work_timer_fn, 0);
	dw->wq = NULL;
}

bool queue_delayed_work (struct workqueue_struct *wq, struct delayed_work *dw, unsigned long delay)
{
	if (work_pending(&dw->work) || timer_pending(&dw->timer))
		return false;
	dw->wq = wq;
	if (!delay)
		return queue_work(wq, &dw->work);
	mod_timer(&dw->timer, jiffies + delay);
	return true;
}

bool mod_delayed_work (struct workqueue_struct *wq, struct delayed_work *dw, unsigned long delay)
{
	bool was = timer_delete(&dw->timer) | cancel_work(&dw->work);

	dw->wq = wq;
	if (!delay)
		queue_work(wq, &dw->work);
	else
		mod_timer(&dw->timer, jiffies + delay);
	return was;
}

bool cancel_delayed_work (struct delayed_work *dw)
{
	bool was = timer_delete(&dw->timer);

	return cancel_work(&dw->work) || was;
}

bool cancel_delayed_work_sync (struct delayed_work *dw)
{
	bool was = timer_delete_sync(&dw->timer);

	return cancel_work_sync(&dw->work) || was;
}

bool flush_delayed_work (struct delayed_work *dw)
{
	if (timer_delete_sync(&dw->timer))
		queue_work(dw->wq ? dw->wq : system_wq, &dw->work);
	return flush_work(&dw->work);
}

bool delayed_work_pending (const struct delayed_work *dw)
{
	return timer_pending(&dw->timer) || work_pending(&dw->work);
}

int rtw_workqueue_init (void)
{
	spin_lock_init(&wq_lock);
	system_wq = alloc_workqueue("events", 0, 0);
	if (!system_wq)
		return -ENOMEM;
	system_long_wq = system_wq;
	system_unbound_wq = system_wq;
	return 0;
}

void rtw_workqueue_exit (void)
{
	destroy_workqueue(system_wq);
	system_wq = system_long_wq = system_unbound_wq = NULL;
}
