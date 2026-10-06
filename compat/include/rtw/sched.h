/*
 * rtw89win Linux compatibility: locking, completions, time, delays, polling, timers, work queues, RCU.
 *   spin_lock / spin_lock_bh         -> DISPATCH_LEVEL spin lock
 *   spin_lock_irq / spin_lock_irqsave -> raised to the device interrupt level (excludes the ISR), see rtwk_spin_lock_irq
 *   mutex                            -> KMUTEX (PASSIVE_LEVEL)
 *   timer_list                       -> KTIMER + KDPC (callback at DISPATCH_LEVEL)
 *   work queues                      -> one PASSIVE_LEVEL kernel thread per queue (compat/workqueue.c)
 *   RCU                              -> readers count themselves in one global counter; synchronize_rcu waits
 *                                       (PASSIVE_LEVEL) until it has been zero (compat/rcu.c; stations come and
 *                                       go while rtw89's NAPI thread reads them)
 * Copyright (c) 2026 Meari
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef RTW_SCHED_H
#define RTW_SCHED_H

#include <rtw/base.h>

/* ---- spin locks ---- */
typedef struct {
	u64 lock;
	u8 irql;
} spinlock_t;
typedef spinlock_t rwlock_t;
#define __SPIN_LOCK_UNLOCKED(x)         { 0, 0 }
#define DEFINE_SPINLOCK(x)              spinlock_t x = __SPIN_LOCK_UNLOCKED(x)
#define spin_lock_init(l)               do { (l)->lock = 0; (l)->irql = 0; } while (0)
#define rwlock_init(l)                  spin_lock_init(l)
static inline void spin_lock (spinlock_t *l) { u8 o = rtwk_spin_lock(&l->lock); l->irql = o; }
static inline void spin_unlock (spinlock_t *l) { rtwk_spin_unlock(&l->lock, l->irql); }
#define spin_lock_bh(l)                 spin_lock(l)
#define spin_unlock_bh(l)               spin_unlock(l)
static inline void spin_lock_irq (spinlock_t *l) { u8 o = rtwk_spin_lock_irq(&l->lock); l->irql = o; }
#define spin_unlock_irq(l)              spin_unlock(l)
#define spin_lock_irqsave(l, flags)     do { (flags) = rtwk_spin_lock_irq(&(l)->lock); } while (0)
#define spin_unlock_irqrestore(l, flags) rtwk_spin_unlock(&(l)->lock, (u8)(flags))
#define read_lock_bh(l)                 spin_lock(l)
#define read_unlock_bh(l)               spin_unlock(l)
#define write_lock_bh(l)                spin_lock(l)
#define write_unlock_bh(l)              spin_unlock(l)
#define local_bh_disable()              do { } while (0)
#define local_bh_enable()               do { } while (0)
/* linux/cleanup.h guards used by rtw89: guard(rcu)() and scoped_guard(spinlock_irqsave, &lock) { ... } */
#define guard(type)                     __rtw_guard_##type      /* guard(rcu)(): with the RCU part below */
struct __rtw_sg_spinlock_irqsave {
	spinlock_t *l;
	unsigned long flags;
};
static inline struct __rtw_sg_spinlock_irqsave __rtw_sg_spinlock_irqsave_enter (spinlock_t *l)
{ struct __rtw_sg_spinlock_irqsave g = { l, 0 }; spin_lock_irqsave(l, g.flags); return g; }
static inline void __rtw_sg_spinlock_irqsave_exit (struct __rtw_sg_spinlock_irqsave *g)
{ spin_unlock_irqrestore(g->l, g->flags); }
#define scoped_guard(type, args...) \
	for (struct __rtw_sg_##type __sg __attribute__((cleanup(__rtw_sg_##type##_exit))) = \
	     __rtw_sg_##type##_enter(args), *__sg_done = NULL; !__sg_done; __sg_done = (void *)1)

#define in_interrupt()                  (rtwk_current_irql() > 1)
#define in_atomic()                     (rtwk_current_irql() > 0)
#define in_softirq()                    (rtwk_current_irql() == 2)

/* ---- mutex ---- */
struct mutex {
	u64 os[RTWK_MUTEX_SIZE / 8];
};
#define mutex_init(m)                   rtwk_mutex_init((m)->os)
#define mutex_lock(m)                   rtwk_mutex_lock((m)->os)
#define mutex_lock_nested(m, s)         rtwk_mutex_lock((m)->os)
#define mutex_unlock(m)                 rtwk_mutex_unlock((m)->os)
#define mutex_trylock(m)                rtwk_mutex_trylock((m)->os)
#define mutex_is_locked(m)              rtwk_mutex_is_locked((m)->os)
#define mutex_destroy(m)                do { } while (0)
#define devm_mutex_init(dev, m)         ({ mutex_init(m); 0; })

/* ---- completion ---- */
struct completion {
	u64 os[RTWK_EVENT_SIZE / 8];
	unsigned int done;
};
#define init_completion(c)              do { rtwk_event_init((c)->os, 1); (c)->done = 0; } while (0)
#define reinit_completion(c)            do { rtwk_event_clear((c)->os); (c)->done = 0; } while (0)
#define complete(c)                     do { (c)->done = 1; rtwk_event_set((c)->os); } while (0)
#define complete_all(c)                 complete(c)
#define completion_done(c)              (READ_ONCE((c)->done) != 0)
#define wait_for_completion(c)          ((void)rtwk_event_wait((c)->os, -1))
#define DECLARE_COMPLETION_ONSTACK(c)   struct completion c; init_completion(&c)

/* ---- time ---- */
#define HZ                              1000
#define NSEC_PER_USEC                   1000L
#define NSEC_PER_MSEC                   1000000L
#define NSEC_PER_SEC                    1000000000LL
#define USEC_PER_MSEC                   1000L
#define USEC_PER_SEC                    1000000L
#define MSEC_PER_SEC                    1000L
#define MAX_JIFFY_OFFSET                ((LONG_MAX >> 1) - 1)
#define jiffies                         ((unsigned long)(rtwk_time_ns() / NSEC_PER_MSEC))
#define get_jiffies_64()                (rtwk_time_ns() / NSEC_PER_MSEC)
#define time_after(a, b)                ((long)((b) - (a)) < 0)
#define time_before(a, b)               time_after(b, a)
#define time_after_eq(a, b)             ((long)((a) - (b)) >= 0)
#define time_before_eq(a, b)            time_after_eq(b, a)
#define time_is_after_jiffies(a)        time_before(jiffies, a)
#define time_is_before_jiffies(a)       time_after(jiffies, a)
#define time_is_after_eq_jiffies(a)     time_before_eq(jiffies, a)
#define time_is_before_eq_jiffies(a)    time_after_eq(jiffies, a)
#define msecs_to_jiffies(m)             ((unsigned long)(m))
#define usecs_to_jiffies(u)             ((unsigned long)DIV_ROUND_UP((u64)(u), 1000))
#define nsecs_to_jiffies(n)             ((unsigned long)((u64)(n) / NSEC_PER_MSEC))
#define jiffies_to_msecs(j)             ((unsigned int)(j))
#define jiffies_to_usecs(j)             ((unsigned int)(j) * 1000U)
#define round_jiffies_relative(j)       (j)
#define round_jiffies_up(j)             (j)
#define ktime_get()                     ((ktime_t)rtwk_time_ns())
#define ktime_get_ns()                  rtwk_time_ns()
#define ktime_get_boottime()            ((ktime_t)rtwk_time_ns())
#define ktime_get_boottime_ns()         rtwk_time_ns()
#define ktime_get_real_seconds()        ((time64_t)(rtwk_time_ns() / NSEC_PER_SEC))
#define ktime_set(s, ns)                ((ktime_t)(s) * NSEC_PER_SEC + (ns))
#define ktime_add(a, b)                 ((a) + (b))
#define ktime_sub(a, b)                 ((a) - (b))
#define ktime_add_ns(k, n)              ((k) + (n))
#define ktime_add_us(k, u)              ((k) + (ktime_t)(u) * NSEC_PER_USEC)
#define ktime_add_ms(k, m)              ((k) + (ktime_t)(m) * NSEC_PER_MSEC)
#define ktime_to_ns(k)                  (k)
#define ktime_to_us(k)                  ((s64)(k) / NSEC_PER_USEC)
#define ktime_to_ms(k)                  ((s64)(k) / NSEC_PER_MSEC)
#define ns_to_ktime(n)                  ((ktime_t)(n))
#define us_to_ktime(u)                  ((ktime_t)(u) * NSEC_PER_USEC)
#define ms_to_ktime(m)                  ((ktime_t)(m) * NSEC_PER_MSEC)
#define ktime_us_delta(a, b)            ktime_to_us(ktime_sub(a, b))
#define ktime_ms_delta(a, b)            ktime_to_ms(ktime_sub(a, b))
#define ktime_before(a, b)              ((a) < (b))
#define ktime_after(a, b)               ((a) > (b))
#define ktime_compare(a, b)             ((a) < (b) ? -1 : (a) > (b) ? 1 : 0)
#define ktime_is_after(a, b)            ktime_after(a, b)

static inline unsigned long wait_for_completion_timeout (struct completion *c, unsigned long timeout)
{
	u64 start = rtwk_time_ns();

	if (!rtwk_event_wait(c->os, (s64)timeout * 1000))
		return 0;
	u64 used = (rtwk_time_ns() - start) / NSEC_PER_MSEC;
	return used >= timeout ? 1 : timeout - (unsigned long)used;
}

/* ---- delays ---- */
#define udelay(us)                      rtwk_udelay((u32)(us))
#define ndelay(ns)                      rtwk_udelay((u32)DIV_ROUND_UP((ns), 1000))
#define mdelay(ms)                      rtwk_udelay((u32)(ms) * 1000)
#define msleep(ms)                      rtwk_usleep((u64)(ms) * 1000)
#define msleep_interruptible(ms)        (msleep(ms), 0)
#define ssleep(s)                       msleep((s) * 1000)
#define usleep_range(lo, hi)            rtwk_usleep((u64)(lo))
#define usleep_range_idle(lo, hi)       rtwk_usleep((u64)(lo))
#define fsleep(us)                      rtwk_usleep((u64)(us))

/* linux/iopoll.h. The contract rtw89 relies on: the result is 0 when cond became true, else -ETIMEDOUT; val holds
 * the last value read, and there is always one more read after the time is up. timeout_us 0 means no limit.
 * read_poll_timeout (may sleep): the clock is sampled before each read, so a read that started after the deadline
 * is the last one. */
#define read_poll_timeout(op, val, cond, sleep_us, timeout_us, sleep_before_read, args...) \
({ \
	u64 __rp_limit_us = (timeout_us); \
	unsigned long __rp_nap_us = (sleep_us); \
	u64 __rp_end_ns = rtwk_time_ns() + __rp_limit_us * NSEC_PER_USEC; \
	int __rp_res = 0; \
	if ((sleep_before_read) && __rp_nap_us) \
		rtwk_usleep(__rp_nap_us); \
	while (1) { \
		bool __rp_late = __rp_limit_us != 0 && rtwk_time_ns() > __rp_end_ns; \
		(val) = op(args); \
		if (cond) \
			break;                          /* done: 0 */ \
		if (__rp_late) { \
			__rp_res = -ETIMEDOUT;          /* this read began after the deadline */ \
			break; \
		} \
		if (__rp_nap_us) \
			rtwk_usleep(__rp_nap_us); \
		cpu_relax(); \
	} \
	__rp_res; \
})
/* read_poll_timeout_atomic (busy waits, any IRQL): no clock; the budget in ns is reduced by each delay (1 us per
 * round when delay_us is 0 and there is a limit; with neither, it just spins). */
#define read_poll_timeout_atomic(op, val, cond, delay_us, timeout_us, delay_before_read, args...) \
({ \
	u64 __rpa_limit_us = (timeout_us); \
	unsigned long __rpa_step_us = (delay_us); \
	u64 __rpa_budget_ns = __rpa_limit_us * NSEC_PER_USEC; \
	int __rpa_res = 0; \
	if ((delay_before_read) && __rpa_step_us) { \
		rtwk_udelay(__rpa_step_us); \
		if (__rpa_limit_us != 0) \
			__rpa_budget_ns -= __rpa_step_us * NSEC_PER_USEC; \
	} \
	while (1) { \
		(val) = op(args); \
		if (cond) \
			break;                          /* done: 0 */ \
		if (__rpa_limit_us != 0 && (s64)__rpa_budget_ns <= 0) { \
			__rpa_res = -ETIMEDOUT;         /* budget used up */ \
			break; \
		} \
		if (__rpa_step_us) { \
			rtwk_udelay(__rpa_step_us); \
			if (__rpa_limit_us != 0) \
				__rpa_budget_ns -= __rpa_step_us * NSEC_PER_USEC; \
		} else if (__rpa_limit_us != 0) { \
			__rpa_budget_ns -= NSEC_PER_USEC; \
			rtwk_udelay(1); \
		} \
		cpu_relax(); \
	} \
	__rpa_res; \
})
/* the readx_ forms: op takes one argument, addr; no wait before the first read */
#define readx_poll_timeout(op, addr, val, cond, sleep_us, timeout_us) \
	read_poll_timeout(op, val, cond, sleep_us, timeout_us, 0, addr)
#define readx_poll_timeout_atomic(op, addr, val, cond, delay_us, timeout_us) \
	read_poll_timeout_atomic(op, val, cond, delay_us, timeout_us, 0, addr)

/* ---- timers ---- */
struct timer_list {
	u64 os[RTWK_TIMER_SIZE / 8];
	void (*function)(struct timer_list *t);
	unsigned long expires;
	unsigned int flags;
	bool pending;
};
void timer_setup (struct timer_list *t, void (*fn)(struct timer_list *), unsigned int flags);
int mod_timer (struct timer_list *t, unsigned long expires);
int timer_delete (struct timer_list *t);
int timer_delete_sync (struct timer_list *t);
#define add_timer(t)                    mod_timer(t, (t)->expires)
#define del_timer(t)                    timer_delete(t)
#define del_timer_sync(t)               timer_delete_sync(t)
#define timer_shutdown_sync(t)          timer_delete_sync(t)
#define timer_pending(t)                READ_ONCE((t)->pending)
#define from_timer(var, timer, field)   container_of(timer, typeof(*var), field)
#define timer_container_of(var, timer, field) container_of(timer, typeof(*var), field)

/* hrtimer is not used by the files we build; only the type appears in headers */
struct hrtimer {
	struct timer_list t;
};

/* ---- work queues ---- */
struct work_struct;
struct workqueue_struct;
typedef void (*work_func_t)(struct work_struct *work);
struct work_struct {
	struct list_head entry;
	work_func_t func;
	struct workqueue_struct *wq;    /* queue it is pending on, NULL when idle */
	struct workqueue_struct *running_on;
	unsigned long data;
};
struct delayed_work {
	struct work_struct work;
	struct timer_list timer;
	struct workqueue_struct *wq;
};
#define WQ_UNBOUND                      (1 << 1)
#define WQ_FREEZABLE                    (1 << 2)
#define WQ_MEM_RECLAIM                  (1 << 3)
#define WQ_HIGHPRI                      (1 << 4)
#define WQ_PERCPU                       (1 << 8)
extern struct workqueue_struct *system_wq;
extern struct workqueue_struct *system_long_wq;
extern struct workqueue_struct *system_unbound_wq;
#define system_percpu_wq                system_wq
#define system_dfl_wq                   system_unbound_wq
struct workqueue_struct *alloc_workqueue (const char *fmt, unsigned int flags, int max_active, ...);
#define alloc_ordered_workqueue(fmt, flags, args...) alloc_workqueue(fmt, flags, 1, ##args)
#define create_singlethread_workqueue(name) alloc_workqueue(name, 0, 1)
void destroy_workqueue (struct workqueue_struct *wq);
void flush_workqueue (struct workqueue_struct *wq);
void __init_work (struct work_struct *w, work_func_t fn);
void __init_delayed_work (struct delayed_work *dw, work_func_t fn);
#define INIT_WORK(w, fn)                __init_work(w, fn)
#define INIT_DELAYED_WORK(dw, fn)       __init_delayed_work(dw, fn)
#define INIT_DELAYED_WORK_DEFERRABLE(dw, fn) __init_delayed_work(dw, fn)
bool queue_work (struct workqueue_struct *wq, struct work_struct *w);
bool queue_delayed_work (struct workqueue_struct *wq, struct delayed_work *dw, unsigned long delay);
bool mod_delayed_work (struct workqueue_struct *wq, struct delayed_work *dw, unsigned long delay);
bool cancel_work (struct work_struct *w);
bool cancel_work_sync (struct work_struct *w);
bool cancel_delayed_work (struct delayed_work *dw);
bool cancel_delayed_work_sync (struct delayed_work *dw);
bool flush_work (struct work_struct *w);
bool flush_delayed_work (struct delayed_work *dw);
bool work_pending (const struct work_struct *w);
bool delayed_work_pending (const struct delayed_work *dw);
int rtw_workqueue_init (void);
void rtw_workqueue_exit (void);
#define schedule_work(w)                queue_work(system_wq, w)
#define schedule_delayed_work(dw, d)    queue_delayed_work(system_wq, dw, d)
#define disable_work_sync(w)            cancel_work_sync(w)
#define disable_delayed_work_sync(dw)   cancel_delayed_work_sync(dw)
static inline struct delayed_work *to_delayed_work (struct work_struct *w) { return container_of(w, struct delayed_work, work); }

/* ---- RCU (compat/rcu.c) ----
 * Readers count themselves in one global counter (they never sleep, as in Linux; nesting is fine).
 * synchronize_rcu() waits at PASSIVE_LEVEL until the counter has been zero once after the call, so every reader
 * that could see an unlinked object has left. It must not be called from inside a read-side section (that would
 * wait for itself), and at DISPATCH_LEVEL it cannot wait: it then only warns. call_rcu()/kfree_rcu() hand the
 * object to a work item on the system queue that waits and then frees it. */
struct rcu_head {
	struct rcu_head *next;
	void (*func)(struct rcu_head *);
	void *obj;                      /* kfree_rcu(): what to free */
};
extern atomic_t rtw_rcu_readers;
static inline void rcu_read_lock (void) { atomic_inc(&rtw_rcu_readers); smp_mb(); }
static inline void rcu_read_unlock (void) { smp_mb(); atomic_dec(&rtw_rcu_readers); }
#define rcu_read_lock_bh()              rcu_read_lock()
#define rcu_read_unlock_bh()            rcu_read_unlock()
/* guard(rcu)(): a read-side section up to the end of the enclosing scope */
static inline void __rtw_guard_rcu_exit (int *g) { (void)g; rcu_read_unlock(); }
#define __rtw_guard_rcu() \
	int __PASTE(__rtw_rcu_guard_, __COUNTER__) __attribute__((cleanup(__rtw_guard_rcu_exit), unused)) = \
	(rcu_read_lock(), 0)
#define rcu_dereference(p)              READ_ONCE(p)
#define rcu_dereference_bh(p)           READ_ONCE(p)
#define rcu_dereference_protected(p, c) (p)
#define rcu_dereference_check(p, c)     READ_ONCE(p)
#define rcu_access_pointer(p)           READ_ONCE(p)
#define rcu_assign_pointer(p, v)        do { smp_wmb(); WRITE_ONCE(p, v); } while (0)
#define RCU_INIT_POINTER(p, v)          WRITE_ONCE(p, v)
#define rcu_replace_pointer(p, v, c)    ({ typeof(p) __o = (p); rcu_assign_pointer(p, v); __o; })
void synchronize_rcu (void);
#define synchronize_net()               synchronize_rcu()
void call_rcu (struct rcu_head *head, void (*fn)(struct rcu_head *));
void rtw_kfree_rcu (struct rcu_head *head, void *obj);
#define kfree_rcu(p, field)             rtw_kfree_rcu(&(p)->field, (p))
#define kfree_rcu_mightsleep(p)         do { synchronize_rcu(); kfree(p); } while (0)
void rcu_barrier (void);                /* returns when the callbacks queued before the call have run */
int rtw_rcu_init (void);
void rtw_rcu_exit (void);

/* ---- wait queues (only wake_up/wait_event_timeout style use) ---- */
struct wait_queue_head {
	u64 os[RTWK_EVENT_SIZE / 8];
};
typedef struct wait_queue_head wait_queue_head_t;
#define init_waitqueue_head(wq)         rtwk_event_init((wq)->os, 1)
#define wake_up(wq)                     rtwk_event_set((wq)->os)
#define wake_up_all(wq)                 rtwk_event_set((wq)->os)
#define wait_event_timeout(wq, cond, timeout) \
({ \
	unsigned long __end = jiffies + (timeout); \
	long __left = (timeout); \
	while (!(cond)) { \
		__left = (long)(__end - jiffies); \
		if (__left <= 0) { __left = (cond) ? 1 : 0; break; } \
		rtwk_event_clear((wq)->os); \
		if (cond) break; \
		rtwk_event_wait((wq)->os, (s64)__left * 1000); \
	} \
	__left > 0 ? __left : ((cond) ? 1 : 0); \
})

#endif
