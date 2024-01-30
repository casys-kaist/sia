import csv
import os
from typing import List

# Configurations
INDEX_LIST=("original", "sia-sw", "ideal")
RUNTIME=30
REPEAT_NUM=1
FG_THREADS=16

DISTRIBUTION_LIST=("UNIFORM_DIST", "SEQUENTIAL_DIST", "LATEST_DIST", "EXPONENTIAL_DIST", "ZIPF_DIST", "HOTSPOT_DIST")
INITIAL_SIZE=10_000_000
TARGET_SIZE=100_000_000
DATASET_SIZE=100_000_000
READ_RATIO=0.5
INSERT_RATIO=0.5

# Run Microbenchmark
for i in range(REPEAT_NUM):
    for index in INDEX_LIST:
        for dist in DISTRIBUTION_LIST:
            print(f"{index}_{dist}_{i}")
            os.system(f"../build/micro_{index}_{dist}       \
                        --fg={FG_THREADS}                   \
                        --runtime={RUNTIME}                 \
                        --read={READ_RATIO}                 \
                        --insert={INSERT_RATIO}             \
                        --initial-size={INITIAL_SIZE}       \
                        --target-size={TARGET_SIZE}         \
                        --table-size={DATASET_SIZE}         \
                        > ../results/request_dist_{index}_{dist}_{i}.txt")

# Parse
def extract_throughput_n_latency(filename: str) -> List[float]:
    througput = 0.0
    latency = 0.0
    with open(filename, 'r') as f:
        while True:
            line = f.readline()
            if line == "": break
            elif "[micro] Throughput(op/s):" in line:
                splitted = line.split()
                througput = float(splitted[-1])
            elif "[micro] Latency:" in line:
                splitted = line.split()
                latency = float(splitted[-1])
    return [througput, latency]

# Export
print("Experiment Results will be exported to ./request_dist.csv")
with open("./request_dist.csv", "w") as f:
    wr = csv.writer(f)
    wr.writerow(['Index', 'Query Distribution', 'Throughput(ops/sec)', 'Latency(us)'])
    for index in INDEX_LIST:
        for dist in DISTRIBUTION_LIST:
            throughput_sum = 0.0
            latency_sum = 0.0
            valid_counter = 0
            for i in range(REPEAT_NUM):
                throughput, latency = extract_throughput_n_latency(f"../results/request_dist_{index}_{dist}_{i}.txt")
                if latency != 0.0:
                    throughput_sum += throughput
                    latency_sum += latency
                    valid_counter += 1
            throughput_avg = throughput_sum / valid_counter if valid_counter != 0 else 0
            latency_avg = latency_sum / valid_counter if valid_counter != 0 else 0
            wr.writerow([index, dist, throughput_avg, latency_avg])

# Cleanup
os.system(f"make -C ../results")