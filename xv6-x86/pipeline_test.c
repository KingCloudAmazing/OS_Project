#include "types.h"
#include "stat.h"
#include "user.h"

#define MAX_STAGES 5

// Helper function to burn CPU cycles (simulates computational work)
void burn_cpu(int iterations) {
    volatile int dummy = 0;
    for (int i = 0; i < iterations; i++) {
        dummy += i;
    }
}

void close_unused_pipes(int pipes[][2], int count,
                        int keep_read, int keep_write) {
    for(int i = 0; i < count; i++) {
        if(pipes[i][0] != keep_read)
            close(pipes[i][0]);

        if(pipes[i][1] != keep_write)
            close(pipes[i][1]);
    }
}

int main(int argc, char *argv[]) {
    int stages;
    int work_per_stage;
    int deadline_duration; // Target execution window in ticks

    int start_tick, end_tick;
    int pipes[MAX_STAGES - 1][2];
    

    // Validate command-line argument count
    if(argc != 4) {
        printf(1, "Usage: pipeline_test <stages> <work> <deadline>\n");
        exit();
    }

    // Parse arguments
    stages = atoi(argv[1]);
    work_per_stage = atoi(argv[2]);
    deadline_duration = atoi(argv[3]);

    // Validate configuration
    if(stages < 2 || stages > MAX_STAGES ||
       work_per_stage <= 0 || work_per_stage > 10000000 ||
       deadline_duration <= 0) {

        printf(1, "Invalid pipeline configuration\n");
        exit();
    }

    printf(1, "Pipeline Configuration:\n");
    printf(1, "Stages: %d\n", stages);
    printf(1, "CPU Work per Stage: %d\n", work_per_stage);
    printf(1, "Deadline Duration: %d ticks\n", deadline_duration);

    // Temporary restriction: execution is still four-stage.
    if(stages != 4) {
        printf(1, "Generic stage execution will be added in the next milestone.\n");
        exit();
    }

    printf(1, "Starting Pipeline Test Harness...\n");

    // Initialize 3 unidirectional pipes
    // if (pipe(p1) < 0 || pipe(p2) < 0 || pipe(p3) < 0) {
    //     printf(1, "Error: Pipe creation failed\n");
    //     exit();
    // }

    for(int i = 0; i < stages - 1; i++) {
        if(pipe(pipes[i]) < 0) {
            printf(1, "Pipe creation failed\n");
            exit();
        }
    }

    // Spawn CPU Hog Process (Unrelated background workload)
    if (fork() == 0) {
        // CPU Hog has NO_DEADLINE by default
        close_unused_pipes(pipes, stages - 1, -1, -1);
        
        burn_cpu(work_per_stage * 10);
        exit();
    }

    // Spawn Stage 1 (Producer / Writer)
    if (fork() == 0) {
        // Close all unused pipe file descriptors
        // close(p1[0]);
        // close(p2[0]); close(p2[1]);
        // close(p3[0]); close(p3[1]);

        close_unused_pipes(pipes, stages - 1, -1, pipes[0][1]);

        burn_cpu(work_per_stage);
        char token = 'X';
        write(pipes[0][1], &token, 1);
        close(pipes[0][1]);
        exit();
    }

    // Spawn Stage 2 (Intermediate Worker)
    if (fork() == 0) {
        // close(p1[1]);
        // close(p2[0]);
        // close(p3[0]); close(p3[1]);
        close_unused_pipes(pipes, stages - 1, pipes[0][0], pipes[1][1]);

        char token;
        read(pipes[0][0], &token, 1); // Blocks until Stage 1 writes
        close(pipes[0][0]);

        burn_cpu(work_per_stage);

        write(pipes[1][1], &token, 1);
        close(pipes[1][1]);
        exit();
    }

    // Spawn Stage 3 (Intermediate Worker)
    if (fork() == 0) {
        // close(p1[0]); close(p1[1]);
        // close(p2[1]);
        // close(p3[0]);
        close_unused_pipes(pipes, stages - 1, pipes[1][0], pipes[2][1]);

        char token;
        read(pipes[1][0], &token, 1); // Blocks until Stage 2 writes
        close(pipes[1][0]);

        burn_cpu(work_per_stage);

        write(pipes[2][1], &token, 1);
        close(pipes[2][1]);
        exit();
    }

    // Spawn Stage 4 (Final Consumer / Reader with Deadline)
    if (fork() == 0) {
        // close(p1[0]); close(p1[1]);
        // close(p2[0]); close(p2[1]);
        // close(p3[1]);
        close_unused_pipes(pipes, stages - 1, pipes[2][0], -1);

        start_tick = uptime();
        uint abs_deadline = start_tick + deadline_duration;

        // Assign deadline (WID=1, absolute deadline tick)
        setworkflow(1, abs_deadline);

        char token;
        read(pipes[2][0], &token, 1); // Blocks until Stage 3 writes
        close(pipes[2][0]);

        burn_cpu(work_per_stage);
        end_tick = uptime();

        int elapsed = end_tick - start_tick;
        int missed = (end_tick > abs_deadline) ? 1 : 0;

        printf(1, "[METRICS] Start=%d | End=%d | Target=%d | Elapsed=%d | Missed=%d\n",
               start_tick, end_tick, abs_deadline, elapsed, missed);
        exit();
    }

    // Parent Process: Close all pipe descriptors and wait
    // close(p1[0]); close(p1[1]);
    // close(p2[0]); close(p2[1]);
    // close(p3[0]); close(p3[1]);
    close_unused_pipes(pipes, stages - 1, -1, -1);

    for (int i = 0; i < 5; i++) {
        wait();
    }

    printf(1, "Pipeline Test Harness Finished.\n");
    exit();
}