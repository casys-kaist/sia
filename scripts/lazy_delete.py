import csv
import os
from typing import List

# Configurations
INDEX="ideal"
RUNTIME=30
REPEAT_NUM=1
FG_THREADS=16

DISTRIBUTION="UNIFORM_DIST"
INITIAL_SIZE=10_000_000
TARGET_SIZE=100_000_000
DATASET_SIZE=100_000_000
READ_RATIO_LIST=(0.95, 0.90, 0.85)
DELETE_RATIO_LIST=(0.05, 0.10, 0.15)

IDEAL_TRAINING_TIME_LIST=(5, 30, 100, 300)

# Run Microbenchmark
for i in range(REPEAT_NUM):
    for read_ratio, delete_ratio in zip(READ_RATIO_LIST, DELETE_RATIO_LIST):
        for training_time in IDEAL_TRAINING_TIME_LIST:
            print(f"{INDEX}_Delete:{delete_ratio}_TrainTime:{training_time}_{i}")
            os.system(f"../build/micro_ideal_UNIFORM_DIST   \
                        --fg={FG_THREADS}                   \
                        --runtime={RUNTIME}                 \
                        --read={read_ratio}                 \
                        --remove={delete_ratio}             \
                        --initial-size={INITIAL_SIZE}       \
                        --target-size={TARGET_SIZE}         \
                        --table-size={DATASET_SIZE}         \
                        > ../results/lazy_delete_{delete_ratio}_{training_time}_{i}.txt")

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
print("Experiment Results will be exported to ./lazy_delete.csv")
with open("./lazy_delete.csv", "w") as f:
    wr = csv.writer(f)
    wr.writerow(['Deletion Ratio', 'Training Interval(sec)', 'Throughput(ops/sec)', 'Latency(us)'])
    for read_ratio, delete_ratio in zip(READ_RATIO_LIST, DELETE_RATIO_LIST):
        for training_time in IDEAL_TRAINING_TIME_LIST:
            throughput_sum = 0.0
            latency_sum = 0.0
            valid_counter = 0
            for i in range(REPEAT_NUM):
                throughput, latency = extract_throughput_n_latency(f"../results/lazy_delete_{delete_ratio}_{training_time}_{i}.txt")
                if latency != 0.0:
                    throughput_sum += throughput
                    latency_sum += latency
                    valid_counter += 1
            throughput_avg = throughput_sum / valid_counter if valid_counter != 0 else 0
            latency_avg = latency_sum / valid_counter if valid_counter != 0 else 0
            wr.writerow([delete_ratio, training_time, throughput_avg, latency_avg])

# Cleanup
os.system(f"make -C ../results")