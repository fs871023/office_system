// SPDX-License-Identifier: GPL-2.0
#include "sched.h"
#include "pelt.h"

int sched_rr_timeslice = RR_TIMESLICE;
int sysctl_sched_rr_timeslice = (MSEC_PER_SEC / HZ) * RR_TIMESLICE;
static const u64 max_rt_runtime = MAX_BW;
static int do_sched_rt_period_timer(struct rt_bandwidth *rt_b, int overrun);
struct rt_bandwidth def_rt_bandwidth;

static enum hrtimer_restart sched_rt_period_timer(struct hrtimer *timer)
{
	struct rt_bandwidth *rt_b =
		container_of(timer, struct rt_bandwidth, rt_period_timer);
	int idle = 0;
	int overrun;

	raw_spin_lock(&rt_b->rt_runtime_lock);
	for (;;) {
		overrun = hrtimer_forward_now(timer, rt_b->rt_period);
		if (!overrun)
			break;

		raw_spin_unlock(&rt_b->rt_runtime_lock);
		idle = do_sched_rt_period_timer(rt_b, overrun);
		raw_spin_lock(&rt_b->rt_runtime_lock);
	}
	if (idle)
		rt_b->rt_period_active = 0;
	raw_spin_unlock(&rt_b->rt_runtime_lock);

	return idle ? HRTIMER_NORESTART : HRTIMER_RESTART;
}

void init_rt_bandwidth(struct rt_bandwidth *rt_b, u64 period, u64 runtime)
{
	rt_b->rt_period = ns_to_ktime(period);
	rt_b->rt_runtime = runtime;

	raw_spin_lock_init(&rt_b->rt_runtime_lock);

	hrtimer_init(&rt_b->rt_period_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
	rt_b->rt_period_timer.function = sched_rt_period_timer;
}

static void start_rt_bandwidth(struct rt_bandwidth *rt_b)
{
	if (!rt_bandwidth_enabled() || rt_b->rt_runtime == RUNTIME_INF)
		return;

	raw_spin_lock(&rt_b->rt_runtime_lock);
	if (!rt_b->rt_period_active) {
		rt_b->rt_period_active = 1;
		hrtimer_forward_now(&rt_b->rt_period_timer, ns_to_ktime(0));
		hrtimer_start_expires(&rt_b->rt_period_timer,
				      HRTIMER_MODE_ABS_PINNED_HARD);
	}
	raw_spin_unlock(&rt_b->rt_runtime_lock);
}

void init_rt_rq(struct rt_rq *rt_rq)
{
	struct rt_prio_array *array;
	int i;

	array = &rt_rq->active;
	for (i = 0; i < MAX_RT_PRIO; i++) {
		INIT_LIST_HEAD(array->queue + i);
		__clear_bit(i, array->bitmap);
	}

	__set_bit(MAX_RT_PRIO, array->bitmap);
	rt_rq->rt_queued = 0;
	rt_rq->rt_time = 0;
	rt_rq->rt_throttled = 0;
	rt_rq->rt_runtime = 0;
	raw_spin_lock_init(&rt_rq->rt_runtime_lock);
}


#define rt_entity_is_task(rt_se) (1)
static inline struct task_struct *rt_task_of(struct sched_rt_entity *rt_se)
{
	return container_of(rt_se, struct task_struct, rt);
}

static inline struct rq *rq_of_rt_rq(struct rt_rq *rt_rq)
{
	return container_of(rt_rq, struct rq, rt);
}

static inline struct rq *rq_of_rt_se(struct sched_rt_entity *rt_se)
{
	struct task_struct *p = rt_task_of(rt_se);

	return task_rq(p);
}

static inline struct rt_rq *rt_rq_of_se(struct sched_rt_entity *rt_se)
{
	struct rq *rq = rq_of_rt_se(rt_se);

	return &rq->rt;
}

void free_rt_sched_group(struct task_group *tg) { }

int alloc_rt_sched_group(struct task_group *tg, struct task_group *parent)
{
	return 1;
}


static void enqueue_top_rt_rq(struct rt_rq *rt_rq);
static void dequeue_top_rt_rq(struct rt_rq *rt_rq);


static inline void sched_rt_rq_enqueue(struct rt_rq *rt_rq)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	if (!rt_rq->rt_nr_running)
		return;

	enqueue_top_rt_rq(rt_rq);
	resched_curr(rq);
}


bool sched_rt_bandwidth_account(struct rt_rq *rt_rq)
{
	struct rt_bandwidth *rt_b = &def_rt_bandwidth;

	return (hrtimer_active(&rt_b->rt_period_timer) ||
		rt_rq->rt_time < rt_b->rt_runtime);
}


static int do_sched_rt_period_timer(struct rt_bandwidth *rt_b, int overrun)
{
	int i, idle = 1, throttled = 0;
	const struct cpumask *span;

	span = cpu_online_mask;
	for_each_cpu(i, span) {
		int enqueue = 0;
		struct rt_rq *rt_rq = &cpu_rq(i)->rt;
		struct rq *rq = rq_of_rt_rq(rt_rq);
		int skip;

		raw_spin_lock(&rt_rq->rt_runtime_lock);
		if (!sched_feat(RT_RUNTIME_SHARE) && rt_rq->rt_runtime != RUNTIME_INF)
			rt_rq->rt_runtime = rt_b->rt_runtime;
		skip = !rt_rq->rt_time && !rt_rq->rt_nr_running;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
		if (skip)
			continue;

		raw_spin_lock(&rq->lock);
		update_rq_clock(rq);

		if (rt_rq->rt_time) {
			u64 runtime;

			raw_spin_lock(&rt_rq->rt_runtime_lock);
			runtime = rt_rq->rt_runtime;
			rt_rq->rt_time -= min(rt_rq->rt_time, overrun*runtime);
			if (rt_rq->rt_throttled && rt_rq->rt_time < runtime) {
				rt_rq->rt_throttled = 0;
				enqueue = 1;

				if (rt_rq->rt_nr_running && rq->curr == rq->idle)
					rq_clock_cancel_skipupdate(rq);
			}
			if (rt_rq->rt_time || rt_rq->rt_nr_running)
				idle = 0;
			raw_spin_unlock(&rt_rq->rt_runtime_lock);
		} else if (rt_rq->rt_nr_running) {
			idle = 0;
			if (!(rt_rq->rt_throttled))
				enqueue = 1;
		}
		if (rt_rq->rt_throttled)
			throttled = 1;

		if (enqueue)
			sched_rt_rq_enqueue(rt_rq);
		raw_spin_unlock(&rq->lock);
	}

	if (!throttled && (!rt_bandwidth_enabled() || rt_b->rt_runtime == RUNTIME_INF))
		return 1;

	return idle;
}


static int sched_rt_runtime_exceeded(struct rt_rq *rt_rq)
{
	u64 runtime = rt_rq->rt_runtime;

	if (rt_rq->rt_throttled)
		return rt_rq->rt_throttled;

	if(runtime >= ktime_to_ns(def_rt_bandwidth.rt_period))
		return 0;

	
	runtime = rt_rq->rt_runtime;
	if (runtime == RUNTIME_INF)
		return 0;

	if (rt_rq->rt_time > runtime) {
		struct rt_bandwidth *rt_b = &def_rt_bandwidth;

		if (likely(rt_b->rt_runtime)) {
			rt_rq->rt_throttled = 1;
			printk_deferred_once("sched: RT throttling activated\n");
		} else {
			rt_rq->rt_time = 0;
		}

		if(rt_rq->rt_throttled){
			dequeue_top_rt_rq(rt_rq);
			return 1;
		}
	}

	return 0;
}


static void update_curr_rt(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct sched_rt_entity *rt_se = &curr->rt;
	u64 delta_exec;
	u64 now;

	if (curr->sched_class != &rt_sched_class)
		return;

	now = rq_clock_task(rq);
	delta_exec = now - curr->se.exec_start;
	if (unlikely((s64)delta_exec <= 0))
		return;

	schedstat_set(curr->se.statistics.exec_max,
		      max(curr->se.statistics.exec_max, delta_exec));

	curr->se.sum_exec_runtime += delta_exec;
	account_group_exec_runtime(curr, delta_exec);

	curr->se.exec_start = now;
	cgroup_account_cputime(curr, delta_exec);

	if (!rt_bandwidth_enabled())
		return;

	for (; rt_se; rt_se = NULL)
	{	
		struct rt_rq *rt_rq = rt_rq_of_se(rt_se);

		if (rt_rq->rt_runtime != RUNTIME_INF) {
			raw_spin_lock(&rt_rq->rt_runtime_lock);
			rt_rq->rt_time += delta_exec;
			if (sched_rt_runtime_exceeded(rt_rq))
				resched_curr(rq);
			raw_spin_unlock(&rt_rq->rt_runtime_lock);
		}
	}
}

static void dequeue_top_rt_rq(struct rt_rq *rt_rq)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	BUG_ON(&rq->rt != rt_rq);

	if (!rt_rq->rt_queued)
		return;

	BUG_ON(!rq->nr_running);

	sub_nr_running(rq, rt_rq->rt_nr_running);
	rt_rq->rt_queued = 0;

}

static void enqueue_top_rt_rq(struct rt_rq *rt_rq)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	BUG_ON(&rq->rt != rt_rq);

	if (rt_rq->rt_queued)
		return;
	if(rt_rq->rt_throttled)
		return;

	if (rt_rq->rt_nr_running) {
		add_nr_running(rq, rt_rq->rt_nr_running);
		rt_rq->rt_queued = 1;
	}
	cpufreq_update_util(rq, 0);
}



static inline void inc_rt_tasks(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	int prio;
	prio = rt_task_of(rt_se)->prio;

	WARN_ON(!rt_prio(prio));
	rt_rq->rt_nr_running += 1;
	struct task_struct *tsk;
	tsk = rt_task_of(rt_se);
	if(tsk->policy == SCHED_RR)
		rt_rq->rr_nr_running += 1;

	start_rt_bandwidth(&def_rt_bandwidth);
}

static inline void dec_rt_tasks(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	int prio;
	prio = rt_task_of(rt_se)->prio;	
	WARN_ON(!rt_prio(prio));	
	WARN_ON(!rt_rq->rt_nr_running);
	rt_rq->rt_nr_running -= 1;
	struct task_struct *tsk;
	tsk = rt_task_of(rt_se);
	if(tsk->policy == SCHED_RR)
		rt_rq->rr_nr_running -= 1;

}


static inline bool move_entity(unsigned int flags)
{
	if ((flags & (DEQUEUE_SAVE | DEQUEUE_MOVE)) == DEQUEUE_SAVE)
		return false;

	return true;
}

static void __delist_rt_entity(struct sched_rt_entity *rt_se, struct rt_prio_array *array)
{
	list_del_init(&rt_se->run_list);
	int prio;
	prio = rt_task_of(rt_se)->prio;

	if (list_empty(array->queue + prio))
		__clear_bit(prio, array->bitmap);

	rt_se->on_list = 0;
}



static void __enqueue_rt_entity(struct sched_rt_entity *rt_se, unsigned int flags)
{
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
	struct rt_prio_array *array = &rt_rq->active;
	struct list_head *queue;
	int prio;
	
	prio = rt_task_of(rt_se)->prio;
	queue = array->queue + prio;	

	if (move_entity(flags)) {
		WARN_ON_ONCE(rt_se->on_list);
		if (flags & ENQUEUE_HEAD){
			list_add(&rt_se->run_list, queue);
		}
		else
			list_add_tail(&rt_se->run_list, queue);

		__set_bit(prio, array->bitmap);
		rt_se->on_list = 1;
	}
	rt_se->on_rq = 1;

	inc_rt_tasks(rt_se, rt_rq);
}

static void __dequeue_rt_entity(struct sched_rt_entity *rt_se, unsigned int flags)
{
	struct rt_rq *rt_rq; 
	struct rt_prio_array *array; 
	rt_rq = rt_rq_of_se(rt_se);
	array = &rt_rq->active;
	
	if (move_entity(flags)) {
		WARN_ON_ONCE(!rt_se->on_list);
		__delist_rt_entity(rt_se, array);
	}
	rt_se->on_rq = 0;

	dec_rt_tasks(rt_se, rt_rq);
}


static void dequeue_rt_stack(struct sched_rt_entity *rt_se, unsigned int flags)
{
	struct sched_rt_entity *back;
	back = NULL;
	
	for (; rt_se; rt_se = NULL)
	{
		rt_se->back = back;
		back = rt_se;
	}

	dequeue_top_rt_rq(rt_rq_of_se(back));

	for (rt_se = back; rt_se; rt_se = rt_se->back) {
		if(rt_se->on_rq)		
			__dequeue_rt_entity(rt_se, flags);
	}
}


static void enqueue_task_rt(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_rt_entity *rt_se = &p->rt;

	if (flags & ENQUEUE_WAKEUP)
		rt_se->timeout = 0;

	struct rq *temp_rq = rq_of_rt_se(rt_se);
	dequeue_rt_stack(rt_se, flags);
	for (; rt_se; rt_se = NULL)
		__enqueue_rt_entity(rt_se, flags);

	enqueue_top_rt_rq(&temp_rq->rt);
	
}

static void dequeue_task_rt(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_rt_entity *rt_se = &p->rt;

	update_curr_rt(rq);

	struct rq *temp_rq = rq_of_rt_se(rt_se);
	dequeue_rt_stack(rt_se, flags);
	for (; rt_se; rt_se = NULL)
	{
		struct rt_rq *rt_rq = NULL;
		if (rt_rq && rt_rq->rt_nr_running)
			__enqueue_rt_entity(rt_se, flags);
	}
	
	enqueue_top_rt_rq(&temp_rq->rt);
}


static void requeue_rt_entity(struct rt_rq *rt_rq, struct sched_rt_entity *rt_se, int head)
{
	if (rt_se->on_rq) {	
		struct rt_prio_array *array = &rt_rq->active;
		int prio;
		prio = rt_task_of(rt_se)->prio;
		struct list_head *queue = array->queue + prio;

		if (head)
			list_move(&rt_se->run_list, queue);
		else
			list_move_tail(&rt_se->run_list, queue);
	}
}

static void requeue_task_rt(struct rq *rq, struct task_struct *p, int head)
{
	struct sched_rt_entity *rt_se = &p->rt;
	struct rt_rq *rt_rq;

	for (; rt_se; rt_se = NULL)
	{
		rt_rq = rt_rq_of_se(rt_se);
		requeue_rt_entity(rt_rq, rt_se, head);
	}
}

static void yield_task_rt(struct rq *rq)
{
	requeue_task_rt(rq, rq->curr, 0);
}

/*
    OS lab 3
    Modify here !!!
*/
static void check_preempt_curr_rt(struct rq *rq, struct task_struct *p, int flags)
{
	if (p->prio < rq->curr->prio) {
		resched_curr(rq);
		return;
	}
	
	// Hint:
	//
	// The task p is current process.
	// We need to check if this new task p has lower prio value(means high priority)
	// Then the schedule need to re-schedule and executes resched_curr() function
	// from core scheduler.
	
	/*
	    Pseudo code:
		
		if (....) > (....)
		    reschedule current task
			return  
	*/
}

static inline void set_next_task_rt(struct rq *rq, struct task_struct *p, bool first)
{
	p->se.exec_start = rq_clock_task(rq);

	if (!first)
		return;

	if (rq->curr->sched_class != &rt_sched_class)
		update_rt_rq_load_avg(rq_clock_pelt(rq), rq, 0);
}

/*
    OS lab 3
	Modify here !
*/
static struct task_struct *pick_next_task_rt(struct rq *rq)
{	
	// If there is no any runable task in scheduler, then cannot pick next runable task 
	if(rq->rt.rt_queued <= 0) return NULL;

	struct sched_rt_entity *rt_se;
	struct rt_rq *rt_rq  = &rq->rt;	
	struct rt_prio_array *array = &rt_rq->active;
	struct list_head *queue_head;
	

	// find the offset of first nonzero priority array
	int prio_offset;	
	prio_offset = sched_find_first_bit(array->bitmap);
	// shift to this priority
	queue_head = array->queue + prio_offset;
	
	
	// Target: get next scheduable entity address
	// Hint: use linux list library and related data structure 
	// Pseudo code: 
        // rt_se = get_member_address_of_run_list_from_queue_head_pointer()
        
	rt_se->run_list.next = queue_head->next;
	// p is next runable task related to scheduable entity
	struct task_struct *p;
	p = container_of(rt_se, struct task_struct, rt);

	set_next_task_rt(rq, p, true);
	return p;
}

static void put_prev_task_rt(struct rq *rq, struct task_struct *p)
{
	update_curr_rt(rq);
	update_rt_rq_load_avg(rq_clock_pelt(rq), rq, 1);
}


static void switched_to_rt(struct rq *rq, struct task_struct *p)
{
	if (task_on_rq_queued(p) && rq->curr != p) {
		if (p->prio < rq->curr->prio && cpu_online(cpu_of(rq)))
			resched_curr(rq);
	}
}

static void prio_changed_rt(struct rq *rq, struct task_struct *p, int oldprio)
{
	if (!task_on_rq_queued(p))
		return;

	if (rq->curr == p) {
		if (oldprio < p->prio)
			resched_curr(rq);
	} else {
		if (p->prio < rq->curr->prio)
			resched_curr(rq);
	}
}

#ifdef CONFIG_POSIX_TIMERS
static void watchdog(struct rq *rq, struct task_struct *p)
{
	unsigned long soft, hard;

	soft = task_rlimit(p, RLIMIT_RTTIME);
	hard = task_rlimit_max(p, RLIMIT_RTTIME);

	if (soft != RLIM_INFINITY) {
		unsigned long next;

		if (p->rt.watchdog_stamp != jiffies) {
			p->rt.timeout++;
			p->rt.watchdog_stamp = jiffies;
		}

		next = DIV_ROUND_UP(min(soft, hard), USEC_PER_SEC/HZ);
		if (p->rt.timeout > next) {
			posix_cputimers_rt_watchdog(&p->posix_cputimers, p->se.sum_exec_runtime);
		}
	}
}
#else
static inline void watchdog(struct rq *rq, struct task_struct *p) { }
#endif



static void task_tick_rt(struct rq *rq, struct task_struct *p, int queued)
{
	struct sched_rt_entity *rt_se = &p->rt;

	update_curr_rt(rq);
	update_rt_rq_load_avg(rq_clock_pelt(rq), rq, 1);

	watchdog(rq, p);

	if (p->policy != SCHED_RR)
		return;

	if (--p->rt.time_slice)
		return;

	p->rt.time_slice = sched_rr_timeslice;

	for (; rt_se; rt_se = NULL)
	{
		if (rt_se->run_list.prev != rt_se->run_list.next) {
			requeue_task_rt(rq, p, 0);
			resched_curr(rq);
			return;
		}
	}
}

/*
	OS lab 3
	Modify here !!!
*/
static unsigned int get_rr_interval_rt(struct rq *rq, struct task_struct *task)
{
    // Hint: 
    // You must need to check task whether is SCHED_RR or others
    // If the task is belong to SCHED_RR, then you need to return the sched_rr_timeslice
    /*
	
	  if ...
	  	 return sched_rr_timeslice
	  else
	     return 0
	  
	*/
	if (task->policy == SCHED_RR)
		return sched_rr_timeslice;
	else
		return 0;
}

const struct sched_class rt_sched_class = {
	.next				= &fair_sched_class,
	.enqueue_task		= enqueue_task_rt,
	.dequeue_task		= dequeue_task_rt,
	.yield_task			= yield_task_rt,
	.check_preempt_curr	= check_preempt_curr_rt,
	.pick_next_task		= pick_next_task_rt,
	.put_prev_task		= put_prev_task_rt,
	.set_next_task      = set_next_task_rt,
	.task_tick			= task_tick_rt,
	.get_rr_interval	= get_rr_interval_rt,
	.prio_changed		= prio_changed_rt,
	.switched_to		= switched_to_rt,
	.update_curr		= update_curr_rt,
};

static int sched_rt_global_constraints(void)
{
	unsigned long flags;
	int i;

	raw_spin_lock_irqsave(&def_rt_bandwidth.rt_runtime_lock, flags);
	for_each_possible_cpu(i) {
		struct rt_rq *rt_rq = &cpu_rq(i)->rt;

		raw_spin_lock(&rt_rq->rt_runtime_lock);
		rt_rq->rt_runtime = global_rt_runtime();
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
	}
	raw_spin_unlock_irqrestore(&def_rt_bandwidth.rt_runtime_lock, flags);

	return 0;
}

static int sched_rt_global_validate(void)
{
	if (sysctl_sched_rt_period <= 0)
		return -EINVAL;

	if ((sysctl_sched_rt_runtime != RUNTIME_INF) &&
		((sysctl_sched_rt_runtime > sysctl_sched_rt_period) ||
		 ((u64)sysctl_sched_rt_runtime *
			NSEC_PER_USEC > max_rt_runtime)))
		return -EINVAL;

	return 0;
}

static void sched_rt_do_global(void)
{
	def_rt_bandwidth.rt_runtime = global_rt_runtime();
	def_rt_bandwidth.rt_period = ns_to_ktime(global_rt_period());
}

int sched_rt_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos)
{
	int old_period, old_runtime;
	static DEFINE_MUTEX(mutex);
	int ret;

	mutex_lock(&mutex);
	old_period = sysctl_sched_rt_period;
	old_runtime = sysctl_sched_rt_runtime;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);

	if (!ret && write) {
		ret = sched_rt_global_validate();
		if (ret)
			goto undo;

		ret = sched_dl_global_validate();
		if (ret)
			goto undo;

		ret = sched_rt_global_constraints();
		if (ret)
			goto undo;

		sched_rt_do_global();
		sched_dl_do_global();
	}
	if (0) {
undo:
		sysctl_sched_rt_period = old_period;
		sysctl_sched_rt_runtime = old_runtime;
	}
	mutex_unlock(&mutex);

	return ret;
}

int sched_rr_handler(struct ctl_table *table, int write, void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;
	static DEFINE_MUTEX(mutex);

	mutex_lock(&mutex);
	ret = proc_dointvec(table, write, buffer, lenp, ppos);

	if (!ret && write) {
		sched_rr_timeslice =
			sysctl_sched_rr_timeslice <= 0 ? RR_TIMESLICE :
			msecs_to_jiffies(sysctl_sched_rr_timeslice);
	}
	mutex_unlock(&mutex);

	return ret;
}
