#include	<u.h>
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	"../port/edf.h"
#include	"errstr.h"
#include	<ptrace.h>

int	nrdy;
Ref	noteidalloc;

ulong delayedscheds;	/* statistics */
long skipscheds;
long preempts;

static Ref pidalloc;
static int	machnoalloc;

struct Procalloc procalloc;

extern Proc* psalloc(void);
extern void pshash(Proc*);
extern void psrelease(Proc*);
extern void psunhash(Proc*);

enum
{
	Scaling=2,
};

static int reprioritize(Proc*);
static void updatecpu(Proc*);
static int schedgain = 30;	/* units in seconds */

static void rebalance(void);
static ulong balancetime;

Schedq	runq[Nrq];
ulong	runvec;

char *statename[] =
{	/* BUG: generate automatically */
	"Dead",
	"Moribund",
	"Ready",
	"Scheding",
	"Running",
	"Queueing",
	"QueueingR",
	"QueueingW",
	"Wakeme",
	"Broken",
	"Stopped",
	"Rendez",
	"Waitrelease",
};

/*
 * Always splhi()'ed.
 */
void
schedinit(void)		/* never returns */
{
	Edf *e;

	setlabel(&m->sched);
	if(up) {
		if((e = up->edf) && (e->flags & Admitted))
			edfrecord(up);
		updatecpu(up);
		m->proc = nil;
		switch(up->state) {
		case Running:
			ready(up);
			break;
		case Moribund:
			up->state = Dead;

			/*
			 * Holding lock from pexit:
			 * 	procalloc
			 */
			mmurelease(up);

			psrelease(up);
			unlock(&procalloc);
			break;
		}
		up->mach = nil;
		coherence();
		up = nil;
	}
	sched();
}

/*
 *  If changing this routine, look also at sleep().  It
 *  contains a copy of the guts of sched().
 */
void
sched(void)
{
	Proc *p;

	if(m->ilockdepth)
		panic("cpu%d: ilockdepth %d, last lock %#p at %#p, sched called from %#p",
			m->machno,
			m->ilockdepth,
			up? up->lastilock: nil,
			(up && up->lastilock)? lockgetpc(up->lastilock): m->ilockpc,
			getcallerpc(&p+2));

	if(up){
		/*
		 * Delay the sched until the process gives up the locks
		 * it is holding.  This avoids dumb lock loops.
		 * Don't delay if the process is Moribund: it called sched to die.
		 */
		if(up->nlocks && up->state != Moribund){
			up->delaysched++;
			delayedscheds++;
			return;
		}
		up->delaysched = 0;

		splhi();

		/* statistics */
		m->cs++;

		procsave(up);
		if(setlabel(&up->sched)){
			procrestore(up);
			spllo();
			return;
		}
		gotolabel(&m->sched);
	}
	p = runproc();
	if(!p->edf){
		updatecpu(p);
		p->priority = reprioritize(p);
	}
	if(p != m->readied)
		m->schedticks = m->ticks + HZ/10;
	m->readied = 0;
	up = p;
	up->state = Running;
	up->mach = m;
	m->proc = up;
	mmuswitch(up);
	gotolabel(&up->sched);
}

int
anyready(void)
{
	return runvec;
}

int
anyhigher(void)
{
	return runvec & ~((1<<(up->priority+1))-1);
}

/*
 *  here once per clock tick to see if we should resched
 */
void
hzsched(void)
{
	/* once a second, rebalance will reprioritize ready procs */
	if(m->machno == 0)
		rebalance();

	/* unless preempted, get to run for at least 100ms */
	if(anyhigher()
	|| (!up->fixedpri && tickscmp(m->ticks, m->schedticks) >= 0 && anyready())){
		m->readied = nil;	/* avoid cooperative scheduling */
		up->delaysched++;
	}
}

/*
 *  here at the end of non-clock interrupts to see if we should preempt the
 *  current process.  Returns 1 if preempted, 0 otherwise.
 */
int
preempted(void)
{
	if(up != nil && up->state == Running &&
	   m->ilockdepth == 0 &&
	   up->nlocks == 0 &&
	   !up->preempted &&
	   anyhigher() &&
	   !active.exiting){
		m->readied = nil;	/* avoid cooperative scheduling */
		up->preempted = 1;
		sched();
		splhi();
		up->preempted = 0;
		return 1;
	}
	return 0;
}

/*
 * Update the cpu time average for this particular process,
 * which is about to change from up -> not up or vice versa.
 * p->lastupdate is the last time an updatecpu happened.
 *
 * The cpu time average is a decaying average that lasts
 * about D clock ticks.  D is chosen to be approximately
 * the cpu time of a cpu-intensive "quick job".  A job has to run
 * for approximately D clock ticks before we home in on its
 * actual cpu usage.  Thus if you manage to get in and get out
 * quickly, you won't be penalized during your burst.  Once you
 * start using your share of the cpu for more than about D
 * clock ticks though, your p->cpu hits 1000 (1.0) and you end up
 * below all the other quick jobs.  Interactive tasks, because
 * they basically always use less than their fair share of cpu,
 * will be rewarded.
 *
 * If the process has not been running, then we want to
 * apply the filter
 *
 *	cpu = cpu * (D-1)/D
 *
 * n times, yielding
 *
 *	cpu = cpu * ((D-1)/D)^n
 *
 * but D is big enough that this is approximately
 *
 * 	cpu = cpu * (D-n)/D
 *
 * so we use that instead.
 *
 * If the process has been running, we apply the filter to
 * 1 - cpu, yielding a similar equation.  Note that cpu is
 * stored in fixed point (* 1000).
 *
 * Updatecpu must be called before changing up, in order
 * to maintain accurate cpu usage statistics.  It can be called
 * at any time to bring the stats for a given proc up-to-date.
 */
static void
updatecpu(Proc *p)
{
	int D, n, t, ocpu;

	if(p->edf)
		return;

	t = sys->ticks*Scaling + Scaling/2;
	n = t - p->lastupdate;
	p->lastupdate = t;

	if(n == 0)
		return;
	D = schedgain*HZ*Scaling;
	if(n > D)
		n = D;

	ocpu = p->cpu;
	if(p != up)
		p->cpu = (ocpu*(D-n))/D;
	else{
		t = 1000 - ocpu;
		t = (t*(D-n))/D;
		p->cpu = 1000 - t;
	}

//iprint("pid %d %s for %d cpu %d -> %d\n", p->pid,p==up?"active":"inactive",n, ocpu,p->cpu);
}

/*
 * On average, p has used p->cpu of a cpu recently.
 * Its fair share is sys.nonline/m->load of a cpu.  If it has been getting
 * too much, penalize it.  If it has been getting not enough, reward it.
 * I don't think you can get much more than your fair share that
 * often, so most of the queues are for using less.  Having a priority
 * of 3 means you're just right.  Having a higher priority (up to p->basepri)
 * means you're not using as much as you could.
 */
static int
reprioritize(Proc *p)
{
	int fairshare, n, load, ratio;

	load = sys->machptr[0]->load;
	if(load == 0)
		return p->basepri;

	/*
	 *  fairshare = 1.000 * ncpu * 1.000/load,
	 * except the decimal point is moved three places
	 * on both load and fairshare.
	 */
	fairshare = (sys->nonline*1000*1000)/load;
	n = p->cpu;
	if(n == 0)
		n = 1;
	ratio = (fairshare+n/2) / n;
	if(ratio > p->basepri)
		ratio = p->basepri;
	if(ratio < 0)
		panic("reprioritize");
//iprint("pid %d cpu %d load %d fair %d pri %d\n", p->pid, p->cpu, load, fairshare, ratio);
	return ratio;
}

/*
 * add a process to a scheduling queue
 */
void
queueproc(Schedq *rq, Proc *p)
{
	int pri;

	pri = rq - runq;
	lock(runq);
	p->priority = pri;
	p->rnext = 0;
	if(rq->tail)
		rq->tail->rnext = p;
	else
		rq->head = p;
	rq->tail = p;
	rq->n++;
	nrdy++;
	runvec |= 1<<pri;
	unlock(runq);
}

/*
 *  try to remove a process from a scheduling queue (called splhi)
 */
Proc*
dequeueproc(Schedq *rq, Proc *tp)
{
	Proc *l, *p;

	if(!canlock(runq))
		return nil;

	/*
	 *  the queue may have changed before we locked runq,
	 *  refind the target process.
	 */
	l = 0;
	for(p = rq->head; p; p = p->rnext){
		if(p == tp)
			break;
		l = p;
	}

	/*
	 *  p->mach==0 only when process state is saved
	 */
	if(p == 0 || p->mach){
		unlock(runq);
		return nil;
	}
	if(p->rnext == 0)
		rq->tail = l;
	if(l)
		l->rnext = p->rnext;
	else
		rq->head = p->rnext;
	if(rq->head == nil)
		runvec &= ~(1<<(rq-runq));
	rq->n--;
	nrdy--;
	if(p->state != Ready)
		print("dequeueproc %s %d %s\n", p->text, p->pid, statename[p->state]);

	unlock(runq);
	return p;
}

/*
 *  ready(p) picks a new priority for a process and sticks it in the
 *  runq for that priority.
 */
void
ready(Proc *p)
{
	Mreg s;
	int pri;
	Schedq *rq;
	void (*pt)(Proc*, int, vlong, vlong);

	s = splhi();
	if(edfready(p)){
		splx(s);
		return;
	}

	if(p->state == Ready)
		panic("double ready");

	if(p != up){
		while(p->mach != nil)
			{}	/* wait for schedinit to finish */

		if(p->wired == nil || p->wired == m)
			m->readied = p;	/* group scheduling */
	}

	updatecpu(p);
	pri = reprioritize(p);
	p->priority = pri;
	rq = &runq[pri];
	p->state = Ready;
	queueproc(rq, p);
	pt = proctrace;
	if(pt)
		pt(p, SReady, 0, 0);
	splx(s);
}

/*
 *  yield the processor and drop our priority
 */
void
yield(void)
{
	if(anyready()){
		/* pretend we just used 1/2 tick */
		up->lastupdate -= Scaling/2;
		sched();
	}
}

/*
 *  recalculate priorities once a second.  We need to do this
 *  since priorities will otherwise only be recalculated when
 *  the running process blocks.
 */
static void
rebalance(void)
{
	Mreg s;
	int pri, npri, t;
	Schedq *rq;
	Proc *p;

	t = m->ticks;
	if(t - balancetime < HZ)
		return;
	balancetime = t;

	for(pri=0, rq=runq; pri<Npriq; pri++, rq++){
another:
		p = rq->head;
		if(p == nil)
			continue;
		if(p->mp != m)
			continue;
		if(pri == p->basepri)
			continue;
		updatecpu(p);
		npri = reprioritize(p);
		if(npri != pri){
			s = splhi();
			p = dequeueproc(rq, p);
			if(p)
				queueproc(&runq[npri], p);
			splx(s);
			goto another;
		}
	}
}

/*
 *  pick a process to run
 */
Proc*
runproc(void)
{
	Schedq *rq;
	Proc *p;
	ulong start, now;
	int i;
	void (*pt)(Proc*, int, vlong, vlong);

	start = perfticks();

	/* cooperative scheduling until the clock ticks */
	if((p=m->readied) && p->mach==0 && p->state==Ready
	&& (p->wired == nil || p->wired == m)
	&& runq[Nrq-1].head == nil && runq[Nrq-2].head == nil){
		skipscheds++;
		rq = &runq[p->priority];
		goto found;
	}

	preempts++;

loop:
	/*
	 *  find a process that last ran on this processor (affinity),
	 *  or one that hasn't moved in a while (load balancing).  Every
	 *  time around the loop affinity goes down.
	 */
	spllo();
	for(i = 0;; i++){
		/*
		 *  find the highest priority target process that this
		 *  processor can run given affinity constraints.
		 *
		 */
		for(rq = &runq[Nrq-1]; rq >= runq; rq--){
			for(p = rq->head; p; p = p->rnext){
				if(p->mp == nil || p->mp == m
				|| (!p->wired && i > 0))
					goto found;
			}
		}

		/* waste time or halt the CPU */
		idlehands();

		/* remember how much time we're here */
		now = perfticks();
		m->perf.inidle += now-start;
		start = now;
	}

found:
	splhi();
	p = dequeueproc(rq, p);
	if(p == nil)
		goto loop;

	p->state = Scheding;
	p->mp = m;

	if(edflock(p)){
		edfrun(p, rq == &runq[PriEdf]);	/* start deadline timer and do admin */
		edfunlock();
	}
	pt = proctrace;
	if(pt)
		pt(p, SRun, 0, 0);
	return p;
}

void
pickmach(Proc *p)
{
	Mach *mp;
	int i;

	if(sys->nmach < 2)
		return;
	for(;;){
		i = (uint)ainc(&machnoalloc)%sys->nmach;
		mp = sys->machptr[i];
		if(mp->online){
			p->mp = mp;
			return;
		}
	}
}

Proc*
newproc(void)
{
	Proc *p;

	p = psalloc();

	p->state = Scheding;
	p->psstate = "New";
	p->mach = 0;
	p->qnext = 0;
	p->nchild = 0;
	p->nwait = 0;
	p->waitq = 0;
	p->parent = 0;
	p->pgrp = 0;
	p->egrp = 0;
	p->fgrp = 0;
	p->rgrp = 0;
	p->pdbg = 0;
	p->kp = 0;
	if(up != nil && up->procctl == Proc_tracesyscall)
		p->procctl = Proc_tracesyscall;
	else
		p->procctl = 0;
	p->syscallq = nil;
	p->notepending = 0;
	p->notedeferred = 0;
	p->ureg = 0;
	p->privatemem = 0;
	p->errstr = p->errbuf0;
	p->syserrstr = p->errbuf1;
	p->errbuf0[0] = '\0';
	p->errbuf1[0] = '\0';
	p->nlocks = 0;
	p->delaysched = 0;
	p->trace = 0;
	kstrdup(&p->user, "*nouser");
	kstrdup(&p->text, "*notext");
	kstrdup(&p->args, "");
	p->nargs = 0;
	p->setargs = 0;
	memset(p->seg, 0, sizeof p->seg);
	p->pid = incref(&pidalloc);
	pshash(p);
	p->noteid = incref(&noteidalloc);
	if(p->pid <= 0 || p->noteid <= 0)
		panic("pidalloc");
	if(p->kstack == 0)
		p->kstack = smalloc(KSTACK);

	/* sched params */
	p->mp = 0;
	p->wired = 0;
	procpriority(p, PriNormal, 0);
	p->cpu = 0;
	p->lastupdate = sys->ticks*Scaling;
	p->edf = nil;

	return p;
}

/*
 * wire this proc to a machine
 */
void
procwired(Proc *p, int bm)
{
	Proc *pp;
	int i;
	char nwired[MACHMAX];
	Mach *wm, *mp;

	if(bm < 0){
		/* pick a machine to wire to */
		memset(nwired, 0, sizeof(nwired));
		p->wired = 0;
		for(i=0; (pp = psincref(i)) != nil; i++){
			wm = pp->wired;
			if(wm && pp->pid)
				nwired[wm->machno]++;
			psdecref(pp);
		}
		bm = 0;
		for(i=0; i<MACHMAX; i++){
			if((mp = sys->machptr[i]) == nil || !mp->online)
				continue;
			if(nwired[i] < nwired[bm])
				bm = i;
		}
	} else {
		/* use the virtual machine requested */
		bm = bm % MACHMAX;
	}

	p->wired = sys->machptr[bm];
	p->mp = p->wired;
}

void
procpriority(Proc *p, int pri, int fixed)
{
	if(pri >= Npriq)
		pri = Npriq - 1;
	else if(pri < 0)
		pri = 0;
	p->basepri = pri;
	p->priority = pri;
	if(fixed){
		p->fixedpri = 1;
	} else {
		p->fixedpri = 0;
	}
}

/*
 *  sleep if a condition is not true.  Another process will
 *  awaken us after it sets the condition.  When we awaken
 *  the condition may no longer be true.
 *
 *  we lock both the process and the rendezvous to keep r->p
 *  and p->r synchronized.
 */
void
sleep(Rendez *r, int (*f)(void*), void *arg)
{
	Mreg s;
	void (*pt)(Proc*, int, vlong, vlong);

	s = splhi();

	if(up->nlocks)
		print("process %d sleeps with %d locks held, last lock %#p locked at pc %#p, sleep called from %#p\n",
			up->pid, up->nlocks, up->lastlock, lockgetpc(up->lastlock), getcallerpc(&r));
	lock(r);
	lock(&up->rlock);
	if(r->p){
		print("double sleep called from %#p, %d %d\n",
			getcallerpc(&r), r->p->pid, up->pid);
		dumpstack();
	}

	/*
	 *  Wakeup only knows there may be something to do by testing
	 *  r->p in order to get something to lock on.
	 *  Flush that information out to memory in case the sleep is
	 *  committed.
	 */
	r->p = up;

	if((*f)(arg) || up->notepending && !up->notedeferred){
		/*
		 *  if condition happened or a note is pending
		 *  never mind
		 */
		r->p = nil;
		unlock(&up->rlock);
		unlock(r);
	} else {
		/*
		 *  now we are committed to
		 *  change state and call scheduler
		 */
		pt = proctrace;
		if(pt)
			pt(up, SSleep, 0, 0);
		up->state = Wakeme;
		up->r = r;

		/* statistics */
		m->cs++;

		procsave(up);
		if(setlabel(&up->sched)) {
			/*
			 *  here when the process is awakened
			 */
			procrestore(up);
			spllo();
		} else {
			/*
			 *  here to go to sleep (i.e. stop Running)
			 */
			unlock(&up->rlock);
			unlock(r);
			gotolabel(&m->sched);
		}
	}

	if(up->notepending && !up->notedeferred) {
		up->notepending = 0;
		splx(s);
		if(up->procctl == Proc_exitme && up->closingfgrp)
			forceclosefgrp();
		error(Eintr);
	}

	splx(s);
}

static int
tfn(void *arg)
{
	return up->trend == nil || up->tfn(arg);
}

void
twakeup(Ureg*, Timer *t)
{
	Proc *p;
	Rendez *trend;

	p = t->ta;
	trend = p->trend;
	p->trend = 0;
	if(trend)
		wakeup(trend);
}

void
tsleep(Rendez *r, int (*fn)(void*), void *arg, long ms)
{
	if (up->tt){
		print("tsleep: timer active: mode %d, tf %#p\n",
			up->tmode, up->tf);
		timerdel(up);
	}
	up->tns = MS2NS(ms);
	up->tf = twakeup;
	up->tmode = Trelative;
	up->ta = up;
	up->trend = r;
	up->tfn = fn;
	timeradd(up);

	if(waserror()){
		timerdel(up);
		nexterror();
	}
	sleep(r, tfn, arg);
	if (up->tt)
		timerdel(up);
	up->twhen = 0;
	poperror();
}

/*
 *  Expects that only one process can call wakeup for any given Rendez.
 *  We hold both locks to ensure that r->p and p->r remain consistent.
 *  Richard Miller has a better solution that doesn't require both to
 *  be held simultaneously, but I'm a paranoid - presotto.
 */
Proc*
wakeup(Rendez *r)
{
	Mreg s;
	Proc *p;

	s = splhi();

	lock(r);
	p = r->p;

	if(p != nil){
		lock(&p->rlock);
		if(p->state != Wakeme || p->r != r)
			panic("wakeup: state");
		r->p = nil;
		p->r = nil;
		ready(p);
		unlock(&p->rlock);
	}
	unlock(r);

	splx(s);

	return p;
}

/*
 *  if waking a sleeping process, this routine must hold both
 *  p->rlock and r->lock.  However, it can't know them in
 *  the same order as wakeup causing a possible lock ordering
 *  deadlock.  We break the deadlock by giving up the p->rlock
 *  lock if we can't get the r->lock and retrying.
 */
int
postnote(Proc *p, int dolock, char *n, int flag)
{
	Mreg s;
	int ret;
	Rendez *r;
	Proc *d, **l;

	if(dolock)
		qlock(&p->debug);

	if(flag != NUser && (p->notify == 0 || p->notified))
		p->nnote = 0;

	ret = 0;
	if(p->nnote < NNOTE) {
		strcpy(p->note[p->nnote].msg, n);
		p->note[p->nnote++].flag = flag;
		ret = 1;
	}
	p->notepending = 1;
	if(dolock)
		qunlock(&p->debug);

	if(p->notedeferred){
		if(flag == NUser)
			return ret;
		/* do not defer fatal errors and kill through ctl */
		p->notedeferred = 0;
	}

	/* this loop is to avoid lock ordering problems. */
	for(;;){
		s = splhi();
		lock(&p->rlock);
		r = p->r;

		/* waiting for a wakeup? */
		if(r == nil)
			break;	/* no */

		/* try for the second lock */
		if(canlock(r)){
			if(p->state != Wakeme || r->p != p)
				panic("postnote: state %d %d %d", r->p != p, p->r != r, p->state);
			p->r = nil;
			r->p = nil;
			ready(p);
			unlock(r);
			break;
		}

		/* give other process time to get out of critical section and try again */
		unlock(&p->rlock);
		splx(s);
		sched();
	}
	unlock(&p->rlock);
	splx(s);

	if(p->state != Rendezvous)
		return ret;

	/* Try and pull out of a rendezvous */
	lock(p->rgrp);
	if(p->state == Rendezvous) {
		p->rendval = ~0;
		l = &REND(p->rgrp, p->rendtag);
		for(d = *l; d; d = d->rendhash) {
			if(d == p) {
				*l = p->rendhash;
				break;
			}
			l = &d->rendhash;
		}
		ready(p);
	}
	unlock(p->rgrp);
	return ret;
}

/*
 * prevent application notes within a control request that must complete
 */
void
notedefer(void)
{
	int s;

	s = splhi();
	if(up->notedeferred)
		panic("notedefer");
	if(up->notepending){
		up->notepending = 0;
		splx(s);
		error(Eintr);
	}
	up->notedeferred = 1;
	splx(s);
}

void
noteallow(void)
{
	up->notedeferred = 0;
	/* trap or a later sleep will process the note */
}

/*
 * weird thing: keep at most NBROKEN around
 */
#define	NBROKEN 4
struct
{
	QLock;
	int	n;
	Proc	*p[NBROKEN];
}broken;

void
addbroken(Proc *p)
{
	qlock(&broken);
	if(broken.n == NBROKEN) {
		ready(broken.p[0]);
		memmove(&broken.p[0], &broken.p[1], sizeof(Proc*)*(NBROKEN-1));
		--broken.n;
	}
	broken.p[broken.n++] = p;
	qunlock(&broken);

	edfstop(up);
	p->state = Broken;
	p->psstate = 0;
	sched();
}

void
unbreak(Proc *p)
{
	int b;

	qlock(&broken);
	for(b=0; b < broken.n; b++)
		if(broken.p[b] == p) {
			broken.n--;
			memmove(&broken.p[b], &broken.p[b+1],
					sizeof(Proc*)*(NBROKEN-(b+1)));
			ready(p);
			break;
		}
	qunlock(&broken);
}

int
freebroken(void)
{
	int i, n;

	qlock(&broken);
	n = broken.n;
	for(i=0; i<n; i++) {
		ready(broken.p[i]);
		broken.p[i] = 0;
	}
	broken.n = 0;
	qunlock(&broken);
	return n;
}

void
pexit(char *exitstr, int freemem)
{
	Proc *p;
	Segment **s, **es;
	long utime, stime;
	Waitq *wq, *f, *next;
	Fgrp *fgrp;
	Egrp *egrp;
	Rgrp *rgrp;
	Pgrp *pgrp;
	Chan *dot;
	void (*pt)(Proc*, int, vlong, vlong);

	if(up->alarm != 0)
		procalarm(0);

	if (up->tt)
		timerdel(up);
	pt = proctrace;
	if(pt)
		pt(up, SDead, 0, 0);

	/* nil out all the resources under lock (free later) */
	qlock(&up->debug);
	fgrp = up->fgrp;
	up->fgrp = nil;
	egrp = up->egrp;
	up->egrp = nil;
	rgrp = up->rgrp;
	up->rgrp = nil;
	pgrp = up->pgrp;
	up->pgrp = nil;
	dot = up->dot;
	up->dot = nil;
	qunlock(&up->debug);

	if(fgrp)
		closefgrp(fgrp);
	if(egrp)
		closeegrp(egrp);
	if(rgrp)
		closergrp(rgrp);
	if(dot)
		cclose(dot);
	if(pgrp)
		closepgrp(pgrp);

	/*
	 * if not a kernel process and have a parent,
	 * do some housekeeping.
	 */
	if(up->kp == 0) {
		p = up->parent;
		if(p == 0) {
			if(exitstr == 0)
				exitstr = "unknown";
			panic("boot process died: %s", exitstr);
		}

		while(waserror())
			;

		wq = smalloc(sizeof(Waitq));
		poperror();

		wq->w.pid = up->pid;
		utime = up->time[TUser] + up->time[TCUser];
		stime = up->time[TSys] + up->time[TCSys];
		wq->w.time[TUser] = tk2ms(utime);
		wq->w.time[TSys] = tk2ms(stime);
		wq->w.time[TReal] = tk2ms(sys->ticks - up->time[TReal]);
		if(exitstr && exitstr[0])
			snprint(wq->w.msg, sizeof(wq->w.msg), "%s %d: %s",
				up->text, up->pid, exitstr);
		else
			wq->w.msg[0] = '\0';

		lock(&p->exl);
		/*
		 * Check that parent is still alive.
		 */
		if(p->pid == up->parentpid && p->state != Broken) {
			p->nchild--;
			p->time[TCUser] += utime;
			p->time[TCSys] += stime;
			/*
			 * If there would be more than 2000 wait records
			 * processes for my parent, then don't leave a wait
			 * record behind.  This helps prevent badly written
			 * daemon processes from accumulating lots of wait
			 * records.
		 	 */
			if(p->nwait < 2000) {
				wq->next = p->waitq;
				p->waitq = wq;
				p->nwait++;
				wq = nil;
				wakeup(&p->waitr);
			}
		}
		unlock(&p->exl);
		if(wq)
			free(wq);
	}

	if(!freemem)
		addbroken(up);

	qlock(&up->seglock);
	es = &up->seg[NSEG];
	for(s = up->seg; s < es; s++) {
		if(*s) {
			putseg(*s);
			*s = 0;
		}
	}
	qunlock(&up->seglock);

	lock(&up->exl);		/* Prevent my children from leaving waits */
	psunhash(up);
	up->pid = 0;
	wakeup(&up->waitr);
	unlock(&up->exl);

	for(f = up->waitq; f; f = next) {
		next = f->next;
		free(f);
	}

	/* release debuggers */
	qlock(&up->debug);
	if(up->syscallq != nil){
		qhangup(up->syscallq, nil);
		up->syscallq = nil;
	}
	if(up->pdbg) {
		wakeup(&up->pdbg->sleep);
		up->pdbg = 0;
	}
	qunlock(&up->debug);

	edfstop(up);
	if(up->edf != nil){
		free(up->edf);
		up->edf = nil;
	}

	/* Sched must not loop for this lock */
	lock(&procalloc);

	up->state = Moribund;
	sched();
	panic("pexit");
}

int
haswaitq(void *x)
{
	Proc *p;

	p = (Proc *)x;
	return p->waitq != 0;
}

int
pwait(Waitmsg *w)
{
	int cpid;
	Waitq *wq;

	if(!canqlock(&up->qwaitr))
		error(Einuse);

	if(waserror()) {
		qunlock(&up->qwaitr);
		nexterror();
	}

	lock(&up->exl);
	if(up->nchild == 0 && up->waitq == 0) {
		unlock(&up->exl);
		error(Enochild);
	}
	unlock(&up->exl);

	sleep(&up->waitr, haswaitq, up);

	lock(&up->exl);
	wq = up->waitq;
	up->waitq = wq->next;
	up->nwait--;
	unlock(&up->exl);

	qunlock(&up->qwaitr);
	poperror();

	if(w)
		memmove(w, &wq->w, sizeof(Waitmsg));
	cpid = wq->w.pid;
	free(wq);

	return cpid;
}

void
dumpaproc(Proc *p)
{
	uintptr bss;
	char *s;

	if(p == 0)
		return;

	bss = 0;
	if(p->seg[BSEG])
		bss = p->seg[BSEG]->top;

	s = p->psstate;
	if(s == 0)
		s = statename[p->state];
	print("%3d:%10s pc %#p dbgpc %#p  %8s (%s) ut %ld st %ld bss %#p qpc %#p nl %d nd %lud lpc %#p pri %lud\n",
		p->pid, p->text, p->pc, dbgpc(p), s, statename[p->state],
		p->time[0], p->time[1], bss, p->qpc, p->nlocks,
		p->delaysched, p->lastlock ? lockgetpc(p->lastlock) : 0, p->priority);
}

void
procdump(void)
{
	int i;
	Proc *p;

	if(up)
		print("up %d\n", up->pid);
	else
		print("no current process\n");
	for(i=0; (p = psincref(i)) != nil; i++) {
		if(p->state != Dead)
			dumpaproc(p);
		psdecref(p);
	}
}

/*
 * must be called in trap, at least on clock interrupt,
 * to check and clear the flushmmu flag set above
 */
void
checkflushmmu(void)
{
	if(m->mmuflush){
		if(up)
			mmuflush();
		m->mmuflush = 0;
	}
}

/*
 *  wait till all processes have flushed their mmu
 *  state about the given segment
 */
void
procflushseg(Segment *s)
{
	int i, ns, nm, nwait;
	Proc *p;
	Mach *mp;

	/*
	 *  tell all processes with the same set
	 *  of pages to flush their mmus
	 */
	nwait = 0;
	for(i=0; (p = psincref(i)) != nil; i++) {
		if(p->state == Dead){
			psdecref(p);
			continue;
		}
		for(ns = 0; ns < NSEG; ns++){
			if(p->seg[ns] == s){
				p->newtlb = 1;
				for(nm = 0; nm < MACHMAX; nm++){
					if((mp = sys->machptr[nm]) == nil || !mp->online)
						continue;
					if(mp->proc == p){
						mp->mmuflush = 1;
						nwait++;
					}
				}
				break;
			}
		}
		psdecref(p);
	}

	if(nwait == 0)
		return;

	/*
	 *  wait for all processors to take a clock interrupt
	 *  and flush their mmu's
	 */
	for(i = 0; i < MACHMAX; i++){
		if((mp = sys->machptr[i]) == nil || !mp->online || mp == m)
			continue;
		while(mp->mmuflush)
			sched();
	}
}

void
scheddump(void)
{
	Proc *p;
	Schedq *rq;

	for(rq = &runq[Nrq-1]; rq >= runq; rq--){
		if(rq->head == 0)
			continue;
		print("rq%ld:", rq-runq);
		for(p = rq->head; p; p = p->rnext)
			print(" %d(%lud)", p->pid, m->ticks - p->readytime);
		print("\n");
		delay(150);
	}
	print("nrdy %d\n", nrdy);
}

void
kproc(char *name, void (*func)(void *), void *arg)
{
	Proc *p;
	static Pgrp *kpgrp;

	while(waserror())
		{}
	p = newproc();
	poperror();

	p->psstate = 0;
	p->procmode = 0640;
	p->kp = 1;

	p->scallnr = up->scallnr;
	memmove(p->arg, up->arg, sizeof(up->arg));
	p->nerrlab = 0;
	p->slash = up->slash;
	p->dot = up->dot;
	if(p->dot)
		incref(p->dot);

	memmove(p->note, up->note, sizeof(p->note));
	p->nnote = up->nnote;
	p->notified = 0;
	p->lastnote = up->lastnote;
	p->notify = up->notify;
	p->ureg = 0;
	p->dbgreg = 0;

	procpriority(p, PriKproc, 0);

	kprocchild(p, func, arg);

	kstrdup(&p->user, eve);
	kstrdup(&p->text, name);
	if(kpgrp == 0)
		kpgrp = newpgrp();
	p->pgrp = kpgrp;
	incref(kpgrp);

	memset(p->time, 0, sizeof(p->time));
	p->time[TReal] = sys->ticks;
//	pickmach(p);
	ready(p);
}

/*
 *  called splhi() by notify().  See comment in notify for the
 *  reasoning.
 */
void
procctl(Proc *p)
{
	Mreg s;
	char *state;

	switch(p->procctl) {
	case Proc_exitbig:
		spllo();
		pexit("Killed: Insufficient physical memory", 1);

	case Proc_exitme:
		spllo();		/* pexit has locks in it */
		pexit("Killed", 1);

	case Proc_traceme:
		if(p->nnote == 0)
			return;
		/* No break */

	case Proc_stopme:
		p->procctl = 0;
		state = p->psstate;
		p->psstate = "Stopped";
		/* free a waiting debugger */
		s = spllo();
		qlock(&p->debug);
		if(p->pdbg) {
			wakeup(&p->pdbg->sleep);
			p->pdbg = 0;
		}
		qunlock(&p->debug);
		splhi();
		p->state = Stopped;
		sched();
		p->psstate = state;
		splx(s);
		return;
	}
}

void
errorf(char *fmt, ...)
{
	va_list arg;
	char buf[PRINTSIZE];

	va_start(arg, fmt);
	vseprint(buf, buf+sizeof(buf), fmt, arg);
	va_end(arg);
	error(buf);
}

void
error(char *err)
{
	if(up == nil)
		panic("error(%s) not in a process", err);
	spllo();

	if(up->nerrlab >= NERR)
		panic("error stack too deep");
	if(err != up->errstr)
		kstrcpy(up->errstr, err, ERRMAX);
	setlabel(&up->errlab[NERR-1]);
	nexterror();
}

void
nexterror(void)
{
	if(up->nerrlab <= 0)
		labtrap("nexterror");
	gotolabel(&up->errlab[--up->nerrlab]);
}

int
labtrap(char *source)
{
	static Lock l;
	int i;

	ilock(&l);
	print("labtrap (%s):\n", source);
	for (i=NERR-1; i>=0; i--)
		if (up->errlab[i].pc)
			print("%d: sp=%#p pc=%#p\n", i, up->errlab[i].sp, up->errlab[i].pc);
	iunlock(&l);
	delay(3*1000);	/* delay to let cons queue drain */
	panic(source);
	return 1;
}

void
labassert(int nerrlab)
{
	if (up->nerrlab != nerrlab)
		labtrap("labassert");
}

void
exhausted(char *resource)
{
	char buf[ERRMAX];

	sprint(buf, "no free %s", resource);
	iprint("%s\n", buf);
	error(buf);
}

/*
 * neither p nor its segments are necessarily locked
 */
ulong
procdatasize(Proc *p, int addstack)
{
	Segment *s;
	ulong tot, l;
	int i, n;

	tot = 0;
	for(i = 1; i < NSEG; i++){
		s = p->seg[i];
		if(s != nil && (i != SSEG || addstack)){
			l = s->top - s->base;
			n = s->ref;
			if(n > 1)
				l /= n;
			if(p->seg[i] == s)	/* ie, hasn't changed meanwhile */
				tot += l;
		}
	}
	return tot;
}

int
cankillproc(Proc *p)
{
	if(p->kp)
		return 0;
	if(p->state == Dead || p->state == Moribund)
		return 0;
	if(p->procctl == Proc_exitbig || p->procctl == Proc_exitme)
		return 0;
	return strcmp(p->user, eve) != 0 || (p->procmode&0222) != 0;
}

enum{
	Nbig=	8
};

static ulong
findbignoteid(void)
{
	int i, nbig;
	ulong l, noteid, noteids[Nbig], sizes[Nbig], t;
	Proc *p;
	int j;

	memset(sizes, 0, sizeof(sizes));
	memset(noteids, 0, sizeof(noteids));
	nbig = 0;
	for(j = 0; (p = psincref(j)) != nil; j++){
		if(cankillproc(p)){
			noteid = p->noteid;
			l = procdatasize(p, 1);
			if(l < 4*MB)
				continue;
			for(i = 0; i < nbig; i++){
				if(noteid == noteids[i]){
					if(l > sizes[i])
						sizes[i] = l;
					break;
				}
				if(l > sizes[i]){
					t = noteids[i]; noteids[i] = noteid; noteid = t;
					t = sizes[i]; sizes[i] = l; l = t;
				}
			}
			if(i == nbig && nbig < nelem(noteids)){
				noteids[nbig] = noteid;
				sizes[nbig] = l;
				nbig++;
			}
		}
		psdecref(p);
	}
	noteid = 0;
	l = 0;
	for(i = 0; i < nbig; i++){
		if(noteids[i] > noteid && sizes[i] > l/2){
			noteid = noteids[i];
			l = sizes[i];
		}
	}
	return noteid;
}

void
killbig(char *why)
{
	ulong noteid;
	Proc *p;
	int i;

	noteid = findbignoteid();
	if(noteid == 0)
		return;
	for(i = 0; (p = psincref(i)) != nil; i++){
		if(p->noteid == noteid && canqlock(&p->debug)){
			if(p->noteid == noteid && cankillproc(p)){
				print("%ud: %s killed: %s\n", p->pid, p->text, why);
				prockill(p, Proc_exitbig, why);
			}
			qunlock(&p->debug);
		}
		psdecref(p);
	}
}

/*
 * must hold the lock p->debug
 */
void
prockill(Proc *p, int ctl, char *why)
{
	switch(p->state) {
	case Broken:
		unbreak(p);
		break;
	case Stopped:
		p->procctl = ctl;
		postnote(p, 0, why, NExit);
		ready(p);
		break;
	default:
		p->procctl = ctl;
		postnote(p, 0, why, NExit);
		break;
	}
}

/*
 *  change ownership to 'new' of all processes owned by 'old'.  Used when
 *  eve changes.
 */
void
renameuser(char *old, char *new)
{
	int i;
	Proc *p;

	for(i = 0; (p = psincref(i)) != nil; i++){
		if(p->user!=nil && strcmp(old, p->user)==0)
			kstrdup(&p->user, new);
		psdecref(p);
	}
}

/*
 *  time accounting called by clock() splhi'd
 */
void
accounttime(void)
{
	Proc *p;
	ulong n, per;
	static ulong nrun;

	p = m->proc;
	if(p) {
		nrun++;
		p->time[p->insyscall]++;
	}

	/* calculate decaying duty cycles */
	n = perfticks();
	per = n - m->perf.last;
	m->perf.last = n;
	per = (m->perf.period*(HZ-1) + per)/HZ;
	if(per != 0)
		m->perf.period = per;

	m->perf.avg_inidle = (m->perf.avg_inidle*(HZ-1)+m->perf.inidle)/HZ;
	m->perf.inidle = 0;

	m->perf.avg_inintr = (m->perf.avg_inintr*(HZ-1)+m->perf.inintr)/HZ;
	m->perf.inintr = 0;

	/* only one processor gets to compute system load averages */
	if(m->machno != 0)
		return;

	/*
	 * calculate decaying load average.
	 * if we decay by (n-1)/n then it takes
	 * n clock ticks to go from load L to .36 L once
	 * things quiet down.  it takes about 5 n clock
	 * ticks to go to zero.  so using HZ means this is
	 * approximately the load over the last second,
	 * with a tail lasting about 5 seconds.
	 */
	n = nrun;
	nrun = 0;
	n = (nrdy+n)*1000;
	m->load = (m->load*(HZ-1)+n)/HZ;
}

