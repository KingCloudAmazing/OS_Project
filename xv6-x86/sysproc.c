#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int
sys_getpid(void)
{
  return myproc()->pid;
}

int
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
int
sys_setworkflow(void)
{
  int wid;
  int deadline_ticks;

  if(argint(0, &wid) < 0 || argint(1, &deadline_ticks) < 0)
    return -1;

  if (wid < 0 || deadline_ticks < 0)
    return -1;

  proc_set_workflow(wid, (uint)deadline_ticks);

  return 0;
}

// Test hooks for edftest: let user programs declare dependencies directly.
// There is no permission check, so any process can donate to any pid.
int
sys_set_waiting_on(void)
{
  int target_pid;
  if(argint(0, &target_pid) < 0)
    return -1;
  return proc_set_waiting_on_pid(target_pid);
}

int
sys_clear_waiting_on(void)
{
  proc_clear_waiting_on(myproc());
  return 0;
}

int
sys_get_effective_deadline(void)
{
  int target_pid;
  if(argint(0, &target_pid) < 0)
    return -1;
  return (int)proc_get_effective_deadline_pid(target_pid);
}
