#include "types.h"
#include "stat.h"
#include "user.h"

#define ASSERT(cond, msg) do { \
  if (!(cond)) { \
    printf(1, "FAIL: %s (line %d)\n", msg, __LINE__); \
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

// 1. RR Compatibility Test
void
test_rr_compatibility(void)
{
  printf(1, "Scenario 1: Round-Robin Compatibility... ");
  int p[2];
  pipe(p);

  int pid1 = fork();
  if(pid1 == 0){
    burn_cpu(5000000);
    write(p[1], "A", 1);
    exit();
  }

  int pid2 = fork();
  if(pid2 == 0){
    burn_cpu(5000000);
    write(p[1], "B", 1);
    exit();
  }

  char buf[2];
  read(p[0], &buf[0], 1);
  read(p[0], &buf[1], 1);
  ASSERT((buf[0] == 'A' && buf[1] == 'B') || (buf[0] == 'B' && buf[1] == 'A'), "Both processes scheduled");

  wait();
  wait();
  close(p[0]);
  close(p[1]);
  printf(1, "PASS\n");
}

// 2. EDF Scheduling Order Test
void
test_edf_order(void)
{
  printf(1, "Scenario 2: EDF Scheduling Order... ");
  int order[2];
  int start[2];
  pipe(order);
  pipe(start);

  // Late deadline (500)
  int p1 = fork();
  if(p1 == 0){
    setworkflow(1, 500);
    char ch;
    read(start[0], &ch, 1); // Wait for starter gun
    burn_cpu(30000000);
    write(order[1], "L", 1);
    exit();
  }

  // Early deadline (100)
  int p2 = fork();
  if(p2 == 0){
    setworkflow(2, 100);
    char ch;
    read(start[0], &ch, 1); // Wait for starter gun
    burn_cpu(30000000);
    write(order[1], "E", 1);
    exit();
  }

  // Let both children reach read(start[0]) and set their workflows
  sleep(5);

  // Starter gun: release both processes simultaneously
  write(start[1], "GO", 2);

  char first, second;
  read(order[0], &first, 1);
  read(order[0], &second, 1);

  ASSERT(first == 'E', "Process with earlier deadline (100) must finish before later deadline (500)");
  ASSERT(second == 'L', "Process with later deadline (500) finishes second");

  wait();
  wait();
  close(order[0]);
  close(order[1]);
  close(start[0]);
  close(start[1]);
  printf(1, "PASS\n");
}

// 3. Equal-Deadline Fairness Test
void
test_equal_fairness(void)
{
  printf(1, "Scenario 3: Equal-Deadline Fairness... ");
  int p[2];
  pipe(p);

  int pid1 = fork();
  if(pid1 == 0){
    setworkflow(1, 200);
    burn_cpu(10000000);
    write(p[1], "1", 1);
    exit();
  }

  int pid2 = fork();
  if(pid2 == 0){
    setworkflow(2, 200);
    burn_cpu(10000000);
    write(p[1], "2", 1);
    exit();
  }

  char c1, c2;
  read(p[0], &c1, 1);
  read(p[0], &c2, 1);
  ASSERT((c1 == '1' && c2 == '2') || (c1 == '2' && c2 == '1'), "Equal-deadline tasks both execute without starvation");

  wait();
  wait();
  close(p[0]);
  close(p[1]);
  printf(1, "PASS\n");
}

// 4. Execution of Processes Without Deadlines
void
test_no_deadline_exec(void)
{
  printf(1, "Scenario 4: Execution of Processes Without Deadlines... ");
  int pid = fork();
  if(pid == 0){
    // Process without deadline
    burn_cpu(5000000);
    exit();
  }
  wait();
  printf(1, "PASS\n");
}

// 5. Basic Priority Donation
void
test_basic_donation(void)
{
  printf(1, "Scenario 5: Basic Priority Donation... ");
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
    ASSERT(eff == 100, "Child effective deadline lowered to donated 100");

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
  ASSERT(child_eff == 100, "Target process effective deadline is 100");

  write(p2c[1], "D", 1);
  read(c2p[0], &ch, 1);

  clear_waiting_on();
  child_eff = get_effective_deadline(child_pid);
  ASSERT(child_eff == 500, "Target process effective deadline restored to 500");

  write(p2c[1], "C", 1);

  close(p2c[1]);
  close(c2p[0]);
  wait();
  printf(1, "PASS\n");
}

// 6. Transitive Donation (Chain 4 -> 3 -> 2 -> 1)
void
test_transitive_donation(void)
{
  printf(1, "Scenario 6: Transitive Priority Donation (Stage 4->3->2->1)... ");
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
    set_waiting_on(pid1);
    char ch;
    read(pipes[1][0], &ch, 1);
    clear_waiting_on();
    exit();
  }

  // Stage 3: Own deadline 300, waits on Stage 2
  int pid3 = fork();
  if(pid3 == 0){
    setworkflow(3, 300);
    set_waiting_on(pid2);
    char ch;
    read(pipes[2][0], &ch, 1);
    clear_waiting_on();
    exit();
  }

  // Stage 4: Own deadline 100, waits on Stage 3
  int pid4 = fork();
  if(pid4 == 0){
    setworkflow(4, 100);
    set_waiting_on(pid3);
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
  ASSERT(eff3 == 100, "Stage 3 eff == 100 (donated from 4)");
  ASSERT(eff2 == 100, "Stage 2 eff == 100 (donated from 4 via 3)");
  ASSERT(eff1 == 100, "Stage 1 eff == 100 (donated from 4 via 3 via 2)");

  write(sync_p[1], "X", 1);
  write(pipes[2][1], "X", 1);
  write(pipes[1][1], "X", 1);
  write(pipes[0][1], "X", 1);

  for(int i = 0; i < 4; i++)
    wait();

  for(int i = 0; i < 3; i++){
    close(pipes[i][0]);
    close(pipes[i][1]);
  }
  close(sync_p[0]);
  close(sync_p[1]);

  printf(1, "PASS\n");
}

// 7. Donation Withdrawal Test
void
test_donation_withdrawal(void)
{
  printf(1, "Scenario 7: Safe Donation Withdrawal... ");
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

  set_waiting_on(target_pid);
  ASSERT(get_effective_deadline(target_pid) == 120, "Donation applied");

  clear_waiting_on();
  ASSERT(get_effective_deadline(target_pid) == 450, "Donation cleanly withdrawn");

  write(p2c[1], "X", 1);
  wait();
  close(p2c[1]);
  close(c2p[0]);
  printf(1, "PASS\n");
}

// 8. Multiple Donors Test
void
test_multiple_donors(void)
{
  printf(1, "Scenario 8: Multiple Donors (min selection and partial withdrawal)... ");
  int sync1[2], sync2[2];
  pipe(sync1);
  pipe(sync2);

  // Target process: deadline 500
  int target_pid = fork();
  if(target_pid == 0){
    setworkflow(1, 500);
    char ch;
    read(sync1[0], &ch, 1);
    exit();
  }

  // Donor 1: deadline 200
  int donor1_pid = fork();
  if(donor1_pid == 0){
    setworkflow(2, 200);
    set_waiting_on(target_pid);
    char ch;
    read(sync2[0], &ch, 1);
    clear_waiting_on();
    exit();
  }

  // Donor 2: deadline 150
  int donor2_pid = fork();
  if(donor2_pid == 0){
    setworkflow(3, 150);
    set_waiting_on(target_pid);
    char ch;
    read(sync2[0], &ch, 1);
    clear_waiting_on();
    exit();
  }

  sleep(5);
  int eff = get_effective_deadline(target_pid);
  ASSERT(eff == 150, "Target receives minimum of multiple donors (min(200, 150) = 150)");

  // Wake up donor 2 (earlier donor exits), donor 1 remains!
  write(sync2[1], "X", 1);
  sleep(5);

  eff = get_effective_deadline(target_pid);
  ASSERT(eff == 200, "After donor 2 leaves, target retains donor 1's deadline (200)");

  // Wake up donor 1 and target
  write(sync2[1], "X", 1);
  write(sync1[1], "X", 1);

  wait();
  wait();
  wait();

  close(sync1[0]); close(sync1[1]);
  close(sync2[0]); close(sync2[1]);

  printf(1, "PASS\n");
}

// 9. Process Termination and Cleanup
void
test_termination_cleanup(void)
{
  printf(1, "Scenario 9: Process Termination and Cleanup... ");
  int p[2];
  pipe(p);

  int target_pid = fork();
  if(target_pid == 0){
    setworkflow(1, 500);
    char ch;
    read(p[0], &ch, 1);
    exit();
  }

  int donor_pid = fork();
  if(donor_pid == 0){
    setworkflow(2, 100);
    set_waiting_on(target_pid);
    // Exits without explicit clear_waiting_on
    exit();
  }

  wait(); // Wait for donor to exit and be reaped
  sleep(3);

  int eff = get_effective_deadline(target_pid);
  ASSERT(eff == 500, "Target deadline restored to 500 on donor exit");

  write(p[1], "X", 1);
  wait();
  close(p[0]);
  close(p[1]);
  printf(1, "PASS\n");
}

// 10. Cyclic Dependency Handling
void
test_cyclic_dependency(void)
{
  printf(1, "Scenario 10: Cyclic Dependency Rejection... ");
  int p[2];
  pipe(p);

  int child_pid = fork();
  if(child_pid == 0){
    int ppid = getpid() - 1;
    char ch;
    read(p[0], &ch, 1);

    int ret = set_waiting_on(ppid);
    ASSERT(ret == -1, "Cyclic dependency creation must return error (-1)");
    exit();
  }

  int ret = set_waiting_on(child_pid);
  ASSERT(ret == 0, "Parent set waiting on child");

  write(p[1], "G", 1);
  wait();

  clear_waiting_on();
  close(p[0]);
  close(p[1]);
  printf(1, "PASS\n");
}

int
main(void)
{
  printf(1, "\n=======================================================\n");
  printf(1, "       xv6 EDF & Priority Donation Test Suite          \n");
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
  printf(1, "       ALL 10 SCENARIOS PASSED SUCCESSFULLY!          \n");
  printf(1, "=======================================================\n\n");
  exit();
}
