#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

int ncpu = 3;
extern uint64 cas(volatile void* addr, int expected, int newval);

struct cpu cpus[NCPU]; 

struct proc proc[NPROC];

struct proc *initproc;

struct concurrentList ready_lists[NCPU];
struct concurrentList unused_list;
struct concurrentList sleeping_list;
struct concurrentList zombie_list;

struct proc* remove_first(struct concurrentList *list);
int remove(struct concurrentList* list, int index);
int insert(struct concurrentList* list, int index);
int get_least_used_cpu();
struct proc* steal_proc();

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl) {
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void
procinit(void)
{
  initlock(&unused_list.walkLock, "unusedWalkLock");
  initlock(&sleeping_list.walkLock, "sleepingWalkLock");
  initlock(&zombie_list.walkLock, "zombieWalkLock");

  unused_list.head = -1;
  unused_list.name = "unusedList";
  unused_list.counter = 0;
  
  sleeping_list.head = -1;
  sleeping_list.name = "sleepingList";
  sleeping_list.counter = 0;

  zombie_list.head = -1;
  zombie_list.name = "zombieList";
  zombie_list.counter = 0;

  struct concurrentList *ready_list;
  for(ready_list = ready_lists; ready_list < &ready_lists[NCPU]; ready_list++) {
    initlock(&ready_list->walkLock, "readyWalkLock");
    ready_list->head = -1;
    ready_list->name = "readyList";
    ready_list->counter = 0;
  }

  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
    initlock(&p->lock, "proc");
    initlock(&p->walkLock, "procWalkLock");
    p->kstack = KSTACK((int) (p - proc));
    p->next = -1;
  }

  for(int i = 0; i < NPROC; i++) {
    p = &proc[i];
    //there is no need to lock p at this point as the value will always be true.
    p->index = i;
    insert(&unused_list, i); 
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid() {
  int oldPid;
  do {
    oldPid = nextpid;
  }
  while(cas(&nextpid, oldPid, oldPid+1));
  return oldPid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p = remove_first(&unused_list); 

  if (p != 0){
    acquire(&p->lock);
    goto found;
  }
  else{
    return 0;
  }
  
found:
  p->pid = allocpid();
  p->state = USED;
  p->next = -1;
  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
  remove(&zombie_list, p->index);
  insert(&unused_list, p->index);
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;
  release(&p->lock);

  int cpu_num = get_least_used_cpu();
  struct concurrentList* ready_list = &ready_lists[cpu_num]; 
  insert(ready_list, p->index);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid, affiliated_cpu;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;

  release(&np->lock);
  
  acquire(&np->walkLock);

  #ifdef OFF
  {
    acquire(&p->walkLock);
    affiliated_cpu = p->affiliated_cpu;
    release(&p->walkLock);
  }
  #endif
  #ifdef ON 
  {
    affiliated_cpu = get_least_used_cpu();
  }
  #endif

  np->affiliated_cpu = affiliated_cpu;
  np->next = -1;
  release(&np->walkLock);

  struct concurrentList* ready_list = &ready_lists[affiliated_cpu];
  insert(ready_list, np->index);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  acquire(&p->walkLock);
  p->affiliated_cpu = 0;
  release(&p->walkLock);
  insert(&zombie_list,p->index);

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  struct concurrentList* ready_list = &ready_lists[cpuid()];
  c->proc = 0;

  for(;;){

    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
    p = remove_first(ready_list);

    if (p != 0){
      acquire(&p->lock);
      if (p->state == RUNNABLE){
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;

        c->proc=p;
        swtch(&c->context, &p->context);
        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
    else{
      #ifdef ON 
      {
        struct proc* p = steal_proc();
        if (p != 0){
          acquire(&p->lock);
          p->affiliated_cpu = cpuid();

          int currentCount = 0;
          do { 
               currentCount  = ready_list->counter;
          }
          while(cas(&(ready_list->counter), currentCount, currentCount+1));

          if (p->state == RUNNABLE){
            // Switch to chosen process.  It is the process's job
            // to release its lock and then reacquire it
            // before jumping back to us.
            p->state = RUNNING;

            c->proc=p;
            swtch(&c->context, &p->context);
            // Process is done running for now.
            // It should have changed its p->state before coming back.
            c->proc = 0;
          }
          release(&p->lock);
        }
      }
      #endif
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;

  acquire(&p->walkLock);
  struct concurrentList* ready_list = &ready_lists[p->affiliated_cpu];
  release(&p->walkLock);
  insert(ready_list, p->index); 

  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  insert(&sleeping_list,p->index);

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  //admit the proccess into the sleeping list.

  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc* curr, *next;
  acquire(&sleeping_list.walkLock);

  if (sleeping_list.head == -1){
    release(&sleeping_list.walkLock);
    return;
  }

  curr = &proc[sleeping_list.head];

  acquire(&curr->walkLock);
  release(&sleeping_list.walkLock);

  while(curr != 0) 
  {
    if(curr->next == -1){
      next = 0;
    }
    else{
      next = &proc[curr->next];
    }

    acquire(&curr->lock);
    if(curr->chan == chan){
      curr->state = RUNNABLE;

      #ifdef ON 
      {
        curr->affiliated_cpu = get_least_used_cpu();
      }
      #endif

      struct concurrentList* ready_list = &ready_lists[curr->affiliated_cpu];
      release(&curr->walkLock);
      remove(&sleeping_list, curr->index);
      insert(ready_list, curr->index);
      acquire(&curr->walkLock);
    }
    release(&curr->lock);
    if(next != 0){
      acquire(&next->walkLock);
    }
    release(&curr->walkLock);
    curr = next;
  }
}


// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;

        #ifdef ON 
        {
          p->affiliated_cpu = get_least_used_cpu();
        }
        #endif

        struct concurrentList* ready_list = &ready_lists[p->affiliated_cpu];
        remove(&sleeping_list, p->index);
        insert(ready_list, p->index);
      }
    }
    release(&p->lock);
  }
  return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;
    printf("\n");

  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s next:%d", p->pid, state, p->name, p->next);
    printf("\n");
      }
}

int
set_cpu(int cpu_num)
{
  struct proc *p = myproc();
  acquire(&p->walkLock);
  p->affiliated_cpu = cpu_num; 
  release(&p->walkLock);
  yield();
  return cpu_num;
}

int 
get_cpu()
{
  struct proc* p = myproc();
  acquire(&p->walkLock);
  int cpu_num = p->affiliated_cpu;
  release(&p->walkLock);
  return cpu_num;
}

int insert(struct concurrentList* list, int index)
{
  struct proc *curr, *pred, *p;

  p = &proc[index];

  acquire(&list->walkLock);

  if (list->head == -1){
    list->head = index;
    acquire(&p->walkLock);
    p->next = -1;
    release(&p->walkLock);
    release(&list->walkLock);
    return 1;
  }

  curr = &proc[list->head];

  acquire(&curr->walkLock);
  release(&list->walkLock);

  while(curr->next != -1){
    pred = curr;
    curr = &proc[pred->next];
    
    acquire(&curr->walkLock);
    release(&pred->walkLock);
  }

  curr->next = index;
  acquire(&p->walkLock);
  release(&curr->walkLock);
  p->next = -1;
  release(&p->walkLock);
  return 1;
}

struct proc* remove_first(struct concurrentList* list)
{
  struct proc* first;
  acquire(&list->walkLock);

  if(list->head == -1){
    release(&list->walkLock);
    return 0;
  }

  first = &proc[list->head];

  acquire(&first->walkLock);
  list->head = first->next;
  release(&first->walkLock);
  release(&list->walkLock);

  return first;
}

int remove(struct concurrentList* list, int index) 
{
  struct proc *pred, *curr;

  acquire(&list->walkLock);
  if(list->head == -1){
    release(&list->walkLock);
    return 0;
  }
  pred = &proc[list->head];

  acquire(&pred->walkLock);
  if(pred->index == index){
    list->head = pred->next;
    pred->next = -1;
    release(&list->walkLock);
    release(&pred->walkLock);
    return 1;
  }
    
  release(&list->walkLock);
  if(pred->next == -1){
    release(&pred->walkLock);
    return 0;
  }

  while (pred->next != -1) {
    curr = &proc[pred->next];
    
    acquire(&curr->walkLock);

    if (index == curr->index) {
      pred->next = curr->next;
      release(&curr->walkLock);
      release(&pred->walkLock);
      return 1;
    }

    release(&pred->walkLock);
    pred = curr;
  }

  release(&curr->walkLock);
  return 0;
} 

int get_least_used_cpu()
{
  int min_index = 0;
  uint64 *min_p = &((&ready_lists[0])->counter);
  uint64 min_val = *min_p;

  do { 
    for(int i = 0; i < ncpu; i++){
      if(min_val > (&ready_lists[i])->counter){
        min_index = i;
        min_p = &((&ready_lists[i])->counter);
        min_val = *min_p;
      }
    }
  }
  while(cas(min_p, min_val, min_val + 1));

  return min_index;
}


int cpu_process_count(int cpu_num)
{
  struct concurrentList* list = &ready_lists[cpu_num];
  return list->counter;
}

struct proc* steal_proc (){
  struct proc* p;
  for(int i = 0; i < ncpu; i++){
    p = remove_first(&ready_lists[i]);
    if(p != 0){
      return p;
    }
  }
  return 0;
}