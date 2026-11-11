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

    int start_tick = 0;
    int end_tick;
    int pipes[MAX_STAGES - 1][2];

    int stage;
    int pid;
    int read_fd;
    int write_fd;
    int created;
    int j;
    int hog_created = 0;

    char token;
    uint abs_deadline = 0;
    

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


    printf(1, "Starting Pipeline Test Harness...\n");

    // Initialize n - 1 unidirectional pipes
    for(int i = 0; i < stages - 1; i++) {
        if(pipe(pipes[i]) < 0) {
            printf(1, "Pipe creation failed\n");
            exit();
        }
    }

    // Spawn unrelated CPU hog
    pid = fork();

    if(pid < 0) {
        printf(1, "Failed to create CPU hog\n");
        close_unused_pipes(pipes, stages - 1, -1, -1);
        exit();
    }

    // Spawn CPU Hog Process (Unrelated background workload)
    if (pid == 0) {
        // CPU Hog has NO_DEADLINE by default
        close_unused_pipes(pipes, stages - 1, -1, -1);
        
        burn_cpu(work_per_stage * 10);
        exit();
    }

    hog_created = 1;

    // Spawn Stage 1 (Producer / Writer)
    // Spawn Stage 2 (Intermediate Worker)
    // Spawn Stage 3 (Intermediate Worker)
    // Spawn Stage 4 (Final Consumer / Reader with Deadline)

    // Create pipeline stages in reverse order.
    // This gives downstream readers an opportunity to block
    // before upstream stages start producing data.

    // Establish one end-to-end deadline for the pipeline.
    start_tick = uptime();
    abs_deadline = start_tick + deadline_duration;
    created = 0;

    for(stage = stages - 1; stage >= 0; stage--) {

        pid = fork();

        if(pid < 0) {
            printf(1, "Failed to create pipeline stage\n");

            close_unused_pipes(pipes, stages - 1, -1, -1);

            // Reap already-created stages and the CPU hog.
            for(j = 0; j < created + hog_created; j++) {
                wait();
            }

            exit();
        }

        if(pid == 0) {

            // Determine which pipe ends this stage needs.
            read_fd = -1;
            write_fd = -1;

            // Every stage except Stage 1 reads from the previous pipe.
            if(stage > 0) {
                read_fd = pipes[stage - 1][0];
            }

            // Every stage except the final stage writes to the next pipe.
            if(stage < stages - 1) {
                write_fd = pipes[stage][1];
            }

            // Close all descriptors that this stage does not need.
            close_unused_pipes(
                pipes,
                stages - 1,
                read_fd,
                write_fd
            );

            // The final stage receives the workflow deadline.
            if(stage == stages - 1) {
                setworkflow(1, abs_deadline);
            }
            
            // Announce this stage as the writer of its output pipe.
            // A zero-length write transfers no data but records writer_pid.
            if(stage < stages - 1) {
                if(write(write_fd, &token, 0) < 0) {
                    printf(1, "Stage %d: Writer registration failed\n",
                        stage + 1);
                    exit();
                }
            }
            // Read data from the previous stage.
            if(stage > 0) {

                if(read(read_fd, &token, 1) != 1) {
                    printf(1, "Stage %d: Read failed\n", stage + 1);
                    exit();
                }

                close(read_fd);
            }
            else {
                // First stage generates the initial token.
                token = 'X';
            }

            printf(1, "Stage %d: Processing\n", stage + 1);

            // Perform the stage's computational workload.
            burn_cpu(work_per_stage);

            // Forward the token to the next stage.
            if(stage < stages - 1) {

                if(write(write_fd, &token, 1) != 1) {
                    printf(1, "Stage %d: Write failed\n", stage + 1);
                    exit();
                }

                close(write_fd);
            }

            // Only the final stage reports completion metrics.
            if(stage == stages - 1) {

                end_tick = uptime();

                int elapsed = end_tick - start_tick;
                int missed = (end_tick > abs_deadline) ? 1 : 0;

                printf(1,
                    "[METRICS] Start=%d | End=%d | Target=%d | Elapsed=%d | Missed=%d\n",
                    start_tick, end_tick, abs_deadline, elapsed, missed);
            }

            exit();
        }

        created++;
    }

    // Parent Process: Close all pipe descriptors and wait
    close_unused_pipes(pipes, stages - 1, -1, -1);

    for(j = 0; j < stages + hog_created; j++) {
        wait();
    }

    printf(1, "Pipeline Test Harness Finished.\n");
    exit();
}