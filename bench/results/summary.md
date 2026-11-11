Deadline: 11 ticks after the pipeline starts. Times are in xv6 timer ticks.

| Scheduler | CPUs | Runs | Misses | Miss rate | Average execution time | Std dev | Min | Max |
|---|---|---|---|---|---|---|---|---|
| RR | 1 | 20 | 20 | 100% | 12.80 | 0.98 | 12 | 14 |
| EDF | 1 | 20 | 6 | 30% | 10.65 | 0.91 | 10 | 12 |
| EDF + Donation | 1 | 20 | 0 | 0% | 5.55 | 0.86 | 5 | 7 |
| RR | 3 | 20 | 0 | 0% | 5.95 | 0.38 | 5 | 7 |
| EDF | 3 | 20 | 0 | 0% | 6.05 | 0.22 | 6 | 7 |
| EDF + Donation | 3 | 20 | 0 | 0% | 6.00 | 0.32 | 5 | 7 |
