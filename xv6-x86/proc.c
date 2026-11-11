#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;
#if SCHED_MODE != SCHED_RR
static int last_sched_idx = 0;
#endif

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);
static void recompute_donations_locked(void);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  p->wid = 0;
  p->deadline = NO_DEADLINE;
  p->donated_deadline = NO_DEADLINE;
  p->waiting_on = 0;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;
  np->wid = curproc->wid;
  np->deadline = curproc->deadline;
  np->donated_deadline = NO_DEADLINE;
  np->waiting_on = 0;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Clean up priority donation dependencies
  if(curproc->waiting_on != 0){
    curproc->waiting_on = 0;
  }
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->waiting_on == curproc)
      p->waiting_on = 0;
  }
  curproc->donated_deadline = NO_DEADLINE;
  recompute_donations_locked();

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->wid = 0;
        p->deadline = NO_DEADLINE;
        p->donated_deadline = NO_DEADLINE;
        p->waiting_on = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Calculate effective deadline of process p.
// min(own deadline, donated deadline).
// Locking requirements: Caller must ensure safe access; holding ptable.lock
// is required when reading or modifying state across other processes.
uint
proc_effective_deadline(struct proc *p)
{
  if(p == 0)
    return NO_DEADLINE;

#if SCHED_MODE == SCHED_EDF_DONATE
  uint eff = p->deadline;
  if(p->donated_deadline != NO_DEADLINE){
    if(eff == NO_DEADLINE || p->donated_deadline < eff)
      eff = p->donated_deadline;
  }
  return eff;
#else
  return p->deadline;
#endif
}

// Recompute donated deadlines across all processes in ptable.
// Propagates transitively across dependency chains until fixed point or bounded NPROC iterations.
// Requires ptable.lock to be held.
static void
recompute_donations_locked(void)
{
  int changed;
  int iter;
  struct proc *p;
  struct proc *w;

  for(iter = 0; iter < NPROC; iter++){
    changed = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state == UNUSED)
        continue;

      uint min_donated = NO_DEADLINE;
      for(w = ptable.proc; w < &ptable.proc[NPROC]; w++){
        if(w->state == UNUSED || w->state == ZOMBIE)
          continue;
        if(w->waiting_on == p){
          uint w_eff = proc_effective_deadline(w);
          if(w_eff != NO_DEADLINE){
            if(min_donated == NO_DEADLINE || w_eff < min_donated)
              min_donated = w_eff;
          }
        }
      }

      if(p->donated_deadline != min_donated){
        p->donated_deadline = min_donated;
        changed = 1;
      }
    }
    if(!changed)
      break;
  }
}

// A process can take part in a dependency only once fork() has finished
// creating it (not EMBRYO) and until it exits (not ZOMBIE).  An EMBRYO
// slot can return to UNUSED if fork() fails, which would leave a waiter
// pointing at a slot that gets reused by an unrelated process.
static int
proc_is_live(struct proc *p)
{
  return p->state == SLEEPING || p->state == RUNNABLE || p->state == RUNNING;
}

// Set dependency: waiter waits on owner.
// Requires ptable.lock to be held.
static int
proc_set_waiting_on_locked(struct proc *waiter, struct proc *owner)
{
  if(waiter == 0 || owner == 0 || waiter == owner)
    return -1;
  if(!proc_is_live(waiter) || !proc_is_live(owner))
    return -1;

  // Cycle detection: traverse owner's waiting_on dependency chain
  struct proc *curr = owner;
  int count = 0;
  while(curr != 0 && count < NPROC){
    if(curr == waiter)
      return -1; // Cycle detected: reject dependency
    curr = curr->waiting_on;
    count++;
  }

  waiter->waiting_on = owner;
  recompute_donations_locked();
  return 0;
}

// Clear dependency: waiter no longer waits on anything.
// Requires ptable.lock to be held.
static void
proc_clear_waiting_on_locked(struct proc *waiter)
{
  if(waiter == 0)
    return;

  waiter->waiting_on = 0;
  recompute_donations_locked();
}

// Public API for Person 3 and kernel modules:
int
proc_set_waiting_on(struct proc *waiter, struct proc *owner)
{
  int ret;
  acquire(&ptable.lock);
  ret = proc_set_waiting_on_locked(waiter, owner);
  release(&ptable.lock);
  return ret;
}

void
proc_clear_waiting_on(struct proc *waiter)
{
  acquire(&ptable.lock);
  proc_clear_waiting_on_locked(waiter);
  release(&ptable.lock);
}

void
proc_recompute_donations(void)
{
  acquire(&ptable.lock);
  recompute_donations_locked();
  release(&ptable.lock);
}

// Set the current process's workflow id and deadline, then propagate the
// new deadline to every process it donates to.
void
proc_set_workflow(int wid, uint deadline)
{
  struct proc *p = myproc();

  acquire(&ptable.lock);
  p->wid = wid;
  p->deadline = deadline;
  recompute_donations_locked();
  release(&ptable.lock);
}

int
proc_set_waiting_on_pid(int target_pid)
{
  struct proc *owner = 0;
  struct proc *cur = myproc();
  struct proc *p;
  int ret;

  if(target_pid <= 0)
    return -1;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == target_pid && p->state != UNUSED && p->state != ZOMBIE){
      owner = p;
      break;
    }
  }
  if(owner == 0){
    release(&ptable.lock);
    return -1;
  }

  ret = proc_set_waiting_on_locked(cur, owner);
  release(&ptable.lock);
  return ret;
}

uint
proc_get_effective_deadline_pid(int target_pid)
{
  struct proc *p;
  uint eff = NO_DEADLINE;

  acquire(&ptable.lock);
  if(target_pid <= 0){
    eff = proc_effective_deadline(myproc());
  } else {
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->pid == target_pid && p->state != UNUSED){
        eff = proc_effective_deadline(p);
        break;
      }
    }
  }
  release(&ptable.lock);
  return eff;
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
#if SCHED_MODE == SCHED_RR
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
#else
    {
      struct proc *best_p = 0;
      uint best_d = NO_DEADLINE;
      int best_idx = -1;
      int i;

      for(i = 0; i < NPROC; i++){
        int idx = (last_sched_idx + 1 + i) % NPROC;
        p = &ptable.proc[idx];
        if(p->state != RUNNABLE)
          continue;

        uint d = proc_effective_deadline(p);

        if(best_p == 0){
          best_p = p;
          best_d = d;
          best_idx = idx;
        } else {
          // If both have valid deadlines, earlier deadline wins
          if(d != NO_DEADLINE && best_d != NO_DEADLINE){
            if(d < best_d){
              best_p = p;
              best_d = d;
              best_idx = idx;
            }
          }
          // Process p has a deadline, but best_p does not: p wins
          else if(d != NO_DEADLINE && best_d == NO_DEADLINE){
            best_p = p;
            best_d = d;
            best_idx = idx;
          }
          // Ties and no-deadline pairs are broken fairly by rotating order (best_p retained)
        }
      }

      if(best_p != 0){
        p = best_p;
        last_sched_idx = best_idx;

        c->proc = p;
        switchuvm(p);
        p->state = RUNNING;

        swtch(&(c->scheduler), p->context);
        switchkvm();

        c->proc = 0;
      }
    }
#endif
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
