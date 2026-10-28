#include "types.h"
#include "stat.h"
#include "user.h"

#define STAGE_WORK 5000000  // Configurable CPU work loop count

// Helper function to burn CPU cycles (simulates computational work)
void burn_cpu(int iterations) {
    volatile int dummy = 0;
    for (int i = 0; i < iterations; i++) {
        dummy += i;
    }
}

int main(int argc, char *argv[]) {
    int p1[2], p2[2], p3[2];
    int start_tick, end_tick;
    int deadline_duration = 200; // Target execution window in ticks

    printf(1, "Starting Pipeline Test Harness...\n");

    // Initialize 3 unidirectional pipes
    if (pipe(p1) < 0 || pipe(p2) < 0 || pipe(p3) < 0) {
        printf(1, "Error: Pipe creation failed\n");
        exit();
    }

    // Spawn CPU Hog Process (Unrelated background workload)
    if (fork() == 0) {
        // CPU Hog has NO_DEADLINE by default
        burn_cpu(STAGE_WORK * 10);
        exit();
    }

    // Spawn Stage 1 (Producer / Writer)
    if (fork() == 0) {
        // Close all unused pipe file descriptors
        close(p1[0]);
        close(p2[0]); close(p2[1]);
        close(p3[0]); close(p3[1]);

        burn_cpu(STAGE_WORK);
        char token = 'X';
        write(p1[1], &token, 1);
        close(p1[1]);
        exit();
    }

    // Spawn Stage 2 (Intermediate Worker)
    if (fork() == 0) {
        close(p1[1]);
        close(p2[0]);
        close(p3[0]); close(p3[1]);

        char token;
        read(p1[0], &token, 1); // Blocks until Stage 1 writes
        close(p1[0]);

        burn_cpu(STAGE_WORK);

        write(p2[1], &token, 1);
        close(p2[1]);
        exit();
    }

    // Spawn Stage 3 (Intermediate Worker)
    if (fork() == 0) {
        close(p1[0]); close(p1[1]);
        close(p2[1]);
        close(p3[0]);

        char token;
        read(p2[0], &token, 1); // Blocks until Stage 2 writes
        close(p2[0]);

        burn_cpu(STAGE_WORK);

        write(p3[1], &token, 1);
        close(p3[1]);
        exit();
    }

    // Spawn Stage 4 (Final Consumer / Reader with Deadline)
    if (fork() == 0) {
        close(p1[0]); close(p1[1]);
        close(p2[0]); close(p2[1]);
        close(p3[1]);

        start_tick = uptime();
        uint abs_deadline = start_tick + deadline_duration;

        // Assign deadline (WID=1, absolute deadline tick)
        //setworkflow(1, abs_deadline);

        char token;
        read(p3[0], &token, 1); // Blocks until Stage 3 writes
        close(p3[0]);

        burn_cpu(STAGE_WORK);
        end_tick = uptime();

        int elapsed = end_tick - start_tick;
        int missed = (end_tick > abs_deadline) ? 1 : 0;

        printf(1, "[METRICS] Start=%d | End=%d | Target=%d | Elapsed=%d | Missed=%d\n",
               start_tick, end_tick, abs_deadline, elapsed, missed);
        exit();
    }

    // Parent Process: Close all pipe descriptors and wait
    close(p1[0]); close(p1[1]);
    close(p2[0]); close(p2[1]);
    close(p3[0]); close(p3[1]);

    for (int i = 0; i < 5; i++) {
        wait();
    }

    printf(1, "Pipeline Test Harness Finished.\n");
    exit();
}