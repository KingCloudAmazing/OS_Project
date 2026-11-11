#include "types.h"
#include "stat.h"
#include "user.h"

// SCHED_MODE comes from the Makefile's CFLAGS; these mirror proc.h.
#define SCHED_RR          0
#define SCHED_EDF         1
#define SCHED_EDF_DONATE  2
#ifndef SCHED_MODE
#define SCHED_MODE SCHED_RR
#endif

#define NO_DEADLINE 0

// Expected effective deadline: the donated value when donation is
// compiled in, otherwise the process's own deadline.
#define EXPECT(donated, own) (SCHED_MODE == SCHED_EDF_DONATE ? (donated) : (own))

// Work per progress mark: long enough to span at least one timer tick.
#define BURN_CHUNK 8000000
#define NMARK      12
#define NORDER     6

static int main_pid;
static int failpipe[2];

// A failing child also reports through failpipe so pass() fails the
// scenario; otherwise only the parent's own assertions would count.
#define ASSERT(cond, msg) do { \
  if (!(cond)) { \
    printf(1, "FAIL: %s (line %d)\n", msg, __LINE__); \
    if (getpid() != main_pid) \
      write(failpipe[1], "F", 1); \
    exit(); \
  } \
} while(0)

// Helper: spin CPU for a given count
void
burn_cpu(int iterations)
{
  volatile int dummy = 0;
  for(int i = 0; i < iterations; i++)
    dummy += i;
}

// Start a scenario with no deadline, so nothing leaks between scenarios
// (fork() copies the parent's deadline into every child).
static void
begin(char *name)
{
  printf(1, "%s... ", name);
  setworkflow(0, NO_DEADLINE);
}

// Write a sentinel so the drain never blocks, then fail if any child
// reported a failed assertion before it.
static void
pass(void)
{
  char ch;
  int failed = 0;

  write(failpipe[1], "S", 1);
  while(read(failpipe[0], &ch, 1) == 1 && ch != 'S')
    failed = 1;
  if(failed){
    printf(1, "FAIL: a child process failed an assertion\n");
    exit();
  }
  printf(1, "PASS\n");
}

static void
skip(char *reason)
{
  printf(1, "SKIP (%s)\n", reason);
}

// Run two equal-work CPU-bound children with the same deadline, released
// together, each writing NMARK progress marks to a shared pipe. With
// with_hog, a third child with an earlier deadline competes as well.
// Returns 1 if both children made progress before either had done half
// its work, i.e. they shared the CPU instead of running one after another.
static int
pair_interleaves(uint deadline, int with_hog)
{
  int marks[2], gun[2];
  char buf[2*NMARK + 1];
  char ch;
  int i, k, n, nproc, seen_a, seen_b, seen_h;

  pipe(marks);
  pipe(gun);
  nproc = with_hog ? 3 : 2;

  for(k = 0; k < nproc; k++){
    if(fork() == 0){
      close(marks[0]);
      close(gun[1]);
      if(k == 2)
        setworkflow(9, 1);  // hog: earliest possible deadline
      else
        setworkflow(k + 1, deadline);
      read(gun[0], &ch, 1);
      if(k == 2){
        burn_cpu(BURN_CHUNK * NMARK);
        write(marks[1], "H", 1);
      } else {
        for(i = 0; i < NMARK; i++){
          burn_cpu(BURN_CHUNK);
          write(marks[1], k ? "b" : "a", 1);
        }
      }
      exit();
    }
  }
  close(marks[1]);
  close(gun[0]);

  sleep(3);  // let every child set its deadline and block on the gun
  write(gun[1], "ggg", nproc);

  n = 0;
  seen_h = 0;
  while(n < 2*NMARK && read(marks[0], &ch, 1) == 1){
    if(ch == 'H')
      seen_h = 1;
    else
      buf[n++] = ch;
  }
  if(with_hog && !seen_h)
    seen_h = read(marks[0], &ch, 1) == 1 && ch == 'H';

  for(k = 0; k < nproc; k++)
    wait();
  close(marks[0]);
  close(gun[1]);

  ASSERT(n == 2*NMARK, "Every equal-priority task ran to completion");
  ASSERT(!with_hog || seen_h, "Deadline task ran to completion");

  seen_a = seen_b = 0;
  for(i = 0; i < NMARK; i++){
    if(buf[i] == 'a')
      seen_a = 1;
    if(buf[i] == 'b')
      seen_b = 1;
  }
  return seen_a && seen_b;
}

// 1. Round-Robin time slicing between processes without deadlines.
// In RR mode this is plain RR; in EDF modes it is the no-deadline fallback.
void
test_rr_compatibility(void)
{
  begin("Scenario 1: Round-Robin Time Slicing (no deadlines)");
  ASSERT(pair_interleaves(NO_DEADLINE, 0),
         "Two no-deadline tasks must share the CPU, not run back to back");
  pass();
}

// 2. EDF Scheduling Order Test
void
test_edf_order(void)
{
  begin("Scenario 2: EDF Scheduling Order");
  if(SCHED_MODE == SCHED_RR){
    skip("needs SCHED_MODE >= 1");
    return;
  }

  int order[2], gun[2];
  int pos[NORDER];
  uint base = uptime() + 1000;
  char ch;
  int i, k;

  pipe(order);
  pipe(gun);

  // Fork the latest deadline first so fork order cannot fake EDF order.
  for(k = NORDER - 1; k >= 0; k--){
    if(fork() == 0){
      setworkflow(k + 1, base + 100*k);
      read(gun[0], &ch, 1);
      burn_cpu(20000000);
      ch = '0' + k;
      write(order[1], &ch, 1);
      exit();
    }
  }

  sleep(5);  // let every child set its deadline and block on the gun
  write(gun[1], "gggggg", NORDER);

  for(i = 0; i < NORDER; i++){
    read(order[0], &ch, 1);
    pos[ch - '0'] = i;
  }
  for(i = 0; i < NORDER; i++)
    wait();

  // With c CPUs the c earliest deadlines run at once and finish in any
  // order among themselves, so only require the earliest task in the first
  // half and the latest in the second half. That holds for up to NORDER/2
  // CPUs; on one CPU EDF finishes them strictly in deadline order.
  ASSERT(pos[0] < NORDER/2, "Earliest-deadline task must finish in the first half");
  ASSERT(pos[NORDER-1] >= NORDER/2, "Latest-deadline task must finish in the second half");

  close(order[0]);
  close(order[1]);
  close(gun[0]);
  close(gun[1]);
  pass();
}

// 3. Equal-Deadline Fairness Test
void
test_equal_fairness(void)
{
  begin("Scenario 3: Equal-Deadline Fairness");
  ASSERT(pair_interleaves(uptime() + 1200, 0),
         "Equal-deadline tasks must share the CPU (rotating tie-break)");
  pass();
}

// 4. Processes without deadlines still run, and rotate among themselves,
// while a deadline task competes with them.
void
test_no_deadline_exec(void)
{
  begin("Scenario 4: Execution of Processes Without Deadlines");
  ASSERT(pair_interleaves(NO_DEADLINE, 1),
         "No-deadline tasks must share the CPU alongside a deadline task");
  pass();
}

// 5. Basic Priority Donation
void
test_basic_donation(void)
{
  begin("Scenario 5: Basic Priority Donation");
  int p2c[2], c2p[2];
  pipe(p2c);
  pipe(c2p);

  int child_pid = fork();
  if(child_pid == 0){
    close(p2c[1]);
    close(c2p[0]);

    setworkflow(1, 500);
    int eff = get_effective_deadline(0);
    ASSERT(eff == 500, "Child initial deadline is 500");

    write(c2p[1], "R", 1); // Child is ready

    char ch;
    read(p2c[0], &ch, 1); // Wait for parent donation
    eff = get_effective_deadline(0);
    ASSERT(eff == EXPECT(100, 500), "Child effective deadline lowered to donated 100");

    write(c2p[1], "V", 1); // Child verified

    read(p2c[0], &ch, 1); // Wait for parent clear
    eff = get_effective_deadline(0);
    ASSERT(eff == 500, "Child effective deadline restored to 500");

    close(p2c[0]);
    close(c2p[1]);
    exit();
  }

  close(p2c[0]);
  close(c2p[1]);

  setworkflow(2, 100);

  char ch;
  read(c2p[0], &ch, 1);

  int ret = set_waiting_on(child_pid);
  ASSERT(ret == 0, "set_waiting_on success");

  int child_eff = get_effective_deadline(child_pid);
  ASSERT(child_eff == EXPECT(100, 500), "Target process effective deadline is 100");

  write(p2c[1], "D", 1);
  read(c2p[0], &ch, 1);

  clear_waiting_on();
  child_eff = get_effective_deadline(child_pid);
  ASSERT(child_eff == 500, "Target process effective deadline restored to 500");

  write(p2c[1], "C", 1);

  close(p2c[1]);
  close(c2p[0]);
  wait();
  pass();
}

// 6. Transitive Donation (Chain 4 -> 3 -> 2 -> 1)
void
test_transitive_donation(void)
{
  begin("Scenario 6: Transitive Priority Donation (Stage 4->3->2->1)");
  int pipes[3][2];
  for(int i = 0; i < 3; i++)
    pipe(pipes[i]);

  int sync_p[2];
  pipe(sync_p);

  // Stage 1 (Root Target): Own deadline 500
  int pid1 = fork();
  if(pid1 == 0){
    setworkflow(1, 500);
    char ch;
    read(pipes[0][0], &ch, 1);
    exit();
  }

  // Stage 2: Own deadline 400, waits on Stage 1
  int pid2 = fork();
  if(pid2 == 0){
    setworkflow(2, 400);
    ASSERT(set_waiting_on(pid1) == 0, "Stage 2 waits on Stage 1");
    char ch;
    read(pipes[1][0], &ch, 1);
    clear_waiting_on();
    exit();
  }

  // Stage 3: Own deadline 300, waits on Stage 2
  int pid3 = fork();
  if(pid3 == 0){
    setworkflow(3, 300);
    ASSERT(set_waiting_on(pid2) == 0, "Stage 3 waits on Stage 2");
    char ch;
    read(pipes[2][0], &ch, 1);
    clear_waiting_on();
    exit();
  }

  // Stage 4: Own deadline 100, waits on Stage 3
  int pid4 = fork();
  if(pid4 == 0){
    setworkflow(4, 100);
    ASSERT(set_waiting_on(pid3) == 0, "Stage 4 waits on Stage 3");
    char ch;
    read(sync_p[0], &ch, 1);
    clear_waiting_on();
    exit();
  }

  sleep(5);

  // Check transitive propagation
  int eff1 = get_effective_deadline(pid1);
  int eff2 = get_effective_deadline(pid2);
  int eff3 = get_effective_deadline(pid3);
  int eff4 = get_effective_deadline(pid4);

  ASSERT(eff4 == 100, "Stage 4 eff == 100");
  ASSERT(eff3 == EXPECT(100, 300), "Stage 3 eff == 100 (donated from 4)");
  ASSERT(eff2 == EXPECT(100, 400), "Stage 2 eff == 100 (donated from 4 via 3)");
  ASSERT(eff1 == EXPECT(100, 500), "Stage 1 eff == 100 (donated from 4 via 3 via 2)");

  // Stage 4 leaves: Stage 3's own 300 must now be what flows down the chain.
  write(sync_p[1], "X", 1);
  ASSERT(wait() == pid4, "Stage 4 exits first");
  ASSERT(get_effective_deadline(pid2) == EXPECT(300, 400), "Stage 2 falls back to 300 from Stage 3");
  ASSERT(get_effective_deadline(pid1) == EXPECT(300, 500), "Stage 1 falls back to 300 via Stage 2");

  write(pipes[2][1], "X", 1);
  write(pipes[1][1], "X", 1);
  write(pipes[0][1], "X", 1);

  for(int i = 0; i < 3; i++)
    wait();

  for(int i = 0; i < 3; i++){
    close(pipes[i][0]);
    close(pipes[i][1]);
  }
  close(sync_p[0]);
  close(sync_p[1]);

  pass();
}

// 7. Donation Withdrawal Test
void
test_donation_withdrawal(void)
{
  begin("Scenario 7: Safe Donation Withdrawal");
  int p2c[2], c2p[2];
  pipe(p2c);
  pipe(c2p);

  int target_pid = fork();
  if(target_pid == 0){
    close(p2c[1]);
    close(c2p[0]);

    setworkflow(1, 450);
    write(c2p[1], "R", 1); // Signal ready

    char ch;
    read(p2c[0], &ch, 1); // Wait for parent to finish verification
    close(p2c[0]);
    close(c2p[1]);
    exit();
  }

  close(p2c[0]);
  close(c2p[1]);

  setworkflow(2, 120);

  char ch;
  read(c2p[0], &ch, 1); // Wait until target is ready with deadline 450

  ASSERT(set_waiting_on(target_pid) == 0, "set_waiting_on success");
  ASSERT(get_effective_deadline(target_pid) == EXPECT(120, 450), "Donation applied");

  clear_waiting_on();
  ASSERT(get_effective_deadline(target_pid) == 450, "Donation cleanly withdrawn");

  write(p2c[1], "X", 1);
  wait();
  close(p2c[1]);
  close(c2p[0]);
  pass();
}

// 8. Multiple Donors Test
void
test_multiple_donors(void)
{
  begin("Scenario 8: Multiple Donors (min selection and partial withdrawal)");
  int hold[2], rel1[2], rel2[2];
  pipe(hold);
  pipe(rel1);
  pipe(rel2);

  // Target process: deadline 500
  int target_pid = fork();
  if(target_pid == 0){
    setworkflow(1, 500);
    char ch;
    read(hold[0], &ch, 1);
    exit();
  }

  // Donor 1: deadline 200. Each donor has its own release pipe so the
  // test, not the scheduler, decides which one leaves first.
  int donor1_pid = fork();
  if(donor1_pid == 0){
    setworkflow(2, 200);
    ASSERT(set_waiting_on(target_pid) == 0, "Donor 1 waits on target");
    char ch;
    read(rel1[0], &ch, 1);
    clear_waiting_on();
    exit();
  }

  // Donor 2: deadline 150
  int donor2_pid = fork();
  if(donor2_pid == 0){
    setworkflow(3, 150);
    ASSERT(set_waiting_on(target_pid) == 0, "Donor 2 waits on target");
    char ch;
    read(rel2[0], &ch, 1);
    clear_waiting_on();
    exit();
  }

  sleep(5);
  int eff = get_effective_deadline(target_pid);
  ASSERT(eff == EXPECT(150, 500), "Target receives minimum of multiple donors (min(200, 150) = 150)");

  // Release donor 2 (the earlier deadline); donor 1 remains.
  write(rel2[1], "X", 1);
  ASSERT(wait() == donor2_pid, "Donor 2 exits");

  eff = get_effective_deadline(target_pid);
  ASSERT(eff == EXPECT(200, 500), "After donor 2 leaves, target retains donor 1's deadline (200)");

  write(rel1[1], "X", 1);
  ASSERT(wait() == donor1_pid, "Donor 1 exits");
  ASSERT(get_effective_deadline(target_pid) == 500, "With no donors left, target is back to 500");

  write(hold[1], "X", 1);
  wait();

  close(hold[0]); close(hold[1]);
  close(rel1[0]); close(rel1[1]);
  close(rel2[0]); close(rel2[1]);

  pass();
}

// 9. Process Termination and Cleanup
void
test_termination_cleanup(void)
{
  begin("Scenario 9: Process Termination and Cleanup");
  int hold[2], ready[2], go[2];
  char ch;
  pipe(hold);
  pipe(ready);
  pipe(go);

  int target_pid = fork();
  if(target_pid == 0){
    setworkflow(1, 500);
    read(hold[0], &ch, 1);
    exit();
  }

  // Donor exits without an explicit clear_waiting_on.
  int donor_pid = fork();
  if(donor_pid == 0){
    setworkflow(2, 100);
    ASSERT(set_waiting_on(target_pid) == 0, "Donor waits on target");
    write(ready[1], "R", 1);
    read(go[0], &ch, 1);
    exit();
  }

  read(ready[0], &ch, 1);
  ASSERT(get_effective_deadline(target_pid) == EXPECT(100, 500), "Donation applied before donor exit");

  write(go[1], "X", 1);
  ASSERT(wait() == donor_pid, "Donor exits and is reaped");

  int eff = get_effective_deadline(target_pid);
  ASSERT(eff == 500, "Target deadline restored to 500 on donor exit");

  // Owner exits while we still wait on it: our link must be dropped, so a
  // new process (which may reuse the owner's slot) inherits no donation.
  setworkflow(3, 100);
  ASSERT(set_waiting_on(target_pid) == 0, "Parent waits on target");
  write(hold[1], "X", 1);
  ASSERT(wait() == target_pid, "Target exits and is reaped");

  int fresh_pid = fork();
  if(fresh_pid == 0){
    setworkflow(4, 700);
    write(ready[1], "R", 1);
    read(go[0], &ch, 1);
    exit();
  }
  read(ready[0], &ch, 1);
  ASSERT(get_effective_deadline(fresh_pid) == 700, "New process inherits no stale donation");
  write(go[1], "X", 1);
  wait();
  clear_waiting_on();

  close(hold[0]); close(hold[1]);
  close(ready[0]); close(ready[1]);
  close(go[0]); close(go[1]);
  pass();
}

// 10. Cyclic Dependency Handling: self, 2-cycle and 3-cycle.
void
test_cyclic_dependency(void)
{
  begin("Scenario 10: Cyclic Dependency Rejection");
  int ready[2], g1[2], g2[2];
  char ch;
  int ppid = getpid();
  pipe(ready);
  pipe(g1);
  pipe(g2);

  setworkflow(1, 100);
  ASSERT(set_waiting_on(ppid) == -1, "Self-dependency must be rejected");

  int c2 = fork();
  if(c2 == 0){
    int c1;
    setworkflow(3, 800);
    write(ready[1], "R", 1);
    read(g2[0], (char*)&c1, sizeof c1);
    // Chain is now parent -> c1 -> c2.
    ASSERT(set_waiting_on(ppid) == -1, "3-cycle (c2 -> parent) must be rejected");
    ASSERT(set_waiting_on(c1) == -1, "2-cycle (c2 -> c1) must be rejected");
    ASSERT(get_effective_deadline(0) == EXPECT(100, 800), "Rejected edges leave the chain intact");
    write(ready[1], "D", 1);
    exit();
  }

  int c1 = fork();
  if(c1 == 0){
    setworkflow(2, 700);
    ASSERT(set_waiting_on(c2) == 0, "c1 waits on c2");
    write(ready[1], "R", 1);
    read(g1[0], &ch, 1);
    ASSERT(set_waiting_on(ppid) == -1, "2-cycle (c1 -> parent) must be rejected");
    write(ready[1], "D", 1);
    read(g1[0], &ch, 1);
    exit();
  }

  read(ready[0], &ch, 1);
  read(ready[0], &ch, 1);
  ASSERT(set_waiting_on(c1) == 0, "Parent waits on c1");

  write(g1[1], "G", 1);
  read(ready[0], &ch, 1);
  write(g2[1], (char*)&c1, sizeof c1);
  read(ready[0], &ch, 1);

  write(g1[1], "X", 1);
  wait();
  wait();
  clear_waiting_on();

  close(ready[0]); close(ready[1]);
  close(g1[0]); close(g1[1]);
  close(g2[0]); close(g2[1]);
  pass();
}

int
main(void)
{
  main_pid = getpid();
  pipe(failpipe);

  printf(1, "\n=======================================================\n");
  printf(1, "       xv6 EDF & Priority Donation Test Suite          \n");
  printf(1, "       SCHED_MODE=%d                                    \n", SCHED_MODE);
  printf(1, "=======================================================\n\n");

  test_rr_compatibility();
  test_edf_order();
  test_equal_fairness();
  test_no_deadline_exec();
  test_basic_donation();
  test_transitive_donation();
  test_donation_withdrawal();
  test_multiple_donors();
  test_termination_cleanup();
  test_cyclic_dependency();

  printf(1, "\n=======================================================\n");
  printf(1, "       ALL SCENARIOS PASSED (skips noted above)        \n");
  printf(1, "=======================================================\n\n");
  exit();
}
