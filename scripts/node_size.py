import csv
import os
from typing import List

# Configurations
INDEX_LIST=("original", "sia-sw",)
RUNTIME=30
REPEAT_NUM=1
FG_THREADS=16

YCSB_DATASET_LIST=("amazon", "memetracker")
YCSB_KEY_LIST=(12, 128)
YCSB_WORKLOAD_LIST=("D", "E")
TWITTER_TRACE_NUMBER_LIST=("12.2", "15.5", "31.1", "37.3")
TWITTER_KEY_LIST=(44, 19, 47, 82)

NODE_ACCURACY_THRESHOLD_LIST=(8, 16, 24, 32)

# Run YCSB
for i in range(REPEAT_NUM):
    for node_accuracy in NODE_ACCURACY_THRESHOLD_LIST:
        for index in INDEX_LIST:
            for dataset, key_len in zip(YCSB_DATASET_LIST, YCSB_KEY_LIST):
                for workload in YCSB_WORKLOAD_LIST:
                    print(f"{index}_{dataset}_{workload}_{node_accuracy}_{i}")
                    os.system(f'../build/PERFORMANCE_{index}_ycsb_{key_len}     \
                                    --fg={FG_THREADS}                           \
                                    --runtime={RUNTIME}                         \
                                    --dataset-name={dataset}                    \
                                    --workload-type={workload}                  \
                                    --sindex-group-err-bound={node_accuracy}    \
                                    --sindex-root-err-bound={node_accuracy}     \
                                    > ../results/node_size_{index}_{dataset}_{workload}_{node_accuracy}_{i}.txt')

# Run Twitter Cache Trace
for i in range(REPEAT_NUM):
    for node_accuracy in NODE_ACCURACY_THRESHOLD_LIST:
        for index in INDEX_LIST:
            for cluster, key_len in zip(TWITTER_TRACE_NUMBER_LIST, TWITTER_KEY_LIST):
                print(f"{index}_{cluster}_{node_accuracy}_{i}")
                os.system(f'../build/PERFORMANCE_{index}_twitter_{key_len}  \
                                    --fg={FG_THREADS}           \
                                    --runtime={RUNTIME}         \
                                    --cluster-number={cluster}  \
                                    > ../results/node_size_{index}_{cluster}_{node_accuracy}_{i}.txt')


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
print("Experiment Results will be exported to ./node_size_ycsb.csv")
with open('./node_size_ycsb.csv', 'w') as f:
    wr = csv.writer(f)
    wr.writerow(['Index', 'Dataset', 'Workload', 'Node Size', 'Throughput(ops/sec)', 'Latency(us)'])
    for index in INDEX_LIST:
        for dataset in YCSB_DATASET_LIST:
            for workload in YCSB_WORKLOAD_LIST:
                for node_accuracy in NODE_ACCURACY_THRESHOLD_LIST:
                    throughput_sum = 0.0
                    latency_sum = 0.0
                    valid_counter = 0
                    for i in range(REPEAT_NUM):
                        throughput, latency = extract_throughput_n_latency(f"../results/node_size_{index}_{dataset}_{workload}_{node_accuracy}_{i}.txt")
                        if latency != 0.0:
                            throughput_sum += throughput
                            latency_sum += latency
                            valid_counter += 1
                    throughput_avg = throughput_sum / valid_counter if valid_counter != 0 else 0
                    latency_avg = latency_sum / valid_counter if valid_counter != 0 else 0
                    wr.writerow([index, dataset, workload, node_accuracy, throughput_avg, latency_avg])

print("Experiment Results will be exported to ./node_size_twitter.csv")
with open('./node_size_twitter.csv', 'w') as f:
    wr = csv.writer(f)
    wr.writerow(['Index', 'Cluster', 'Node Size', 'Throughput(ops/sec)', 'Latency(us)'])
    for index in INDEX_LIST:
        for cluster in TWITTER_TRACE_NUMBER_LIST:
            for node_accuracy in NODE_ACCURACY_THRESHOLD_LIST:
                throughput_sum = 0.0
                latency_sum = 0.0
                valid_counter = 0
                for i in range(REPEAT_NUM):
                    throughput, latency = extract_throughput_n_latency(f"../results/node_size_{index}_{cluster}_{node_accuracy}_{i}.txt")
                    if latency != 0.0:
                        throughput_sum += throughput
                        latency_sum += latency
                        valid_counter += 1
                throughput_avg = throughput_sum / valid_counter if valid_counter != 0 else 0
                latency_avg = latency_sum / valid_counter if valid_counter != 0 else 0
                wr.writerow([index, cluster, node_accuracy, throughput_avg, latency_avg])

# Cleanup
os.system(f"make -C ../results")