import csv
import os
from typing import List
from math import isnan

# Configurations
INDEX_LIST=("original",)
RUNTIME=30
REPEAT_NUM=1
FG_THREADS=16

YCSB_DATASET_LIST=("amazon", "memetracker")
YCSB_KEY_LIST=(12, 128)
YCSB_WORKLOAD_LIST=("D", "E")
TWITTER_TRACE_NUMBER_LIST=("12.2", "15.5", "31.1", "37.3")
TWITTER_KEY_LIST=(44, 19, 47, 82)

# Run YCSB
for i in range(REPEAT_NUM):
    for index in INDEX_LIST:
        for dataset, key_len in zip(YCSB_DATASET_LIST, YCSB_KEY_LIST):
            for workload in YCSB_WORKLOAD_LIST:
                print(f"{index}_{dataset}_{workload}_{i}")
                os.system(f'../build/LATENCY_BREAKDOWN_{index}_ycsb_{key_len} \
                                --fg={FG_THREADS}           \
                                --runtime={RUNTIME}         \
                                --dataset-name={dataset}    \
                                --workload-type={workload}  \
                                > ../results/latency_breakdown_{index}_{dataset}_{workload}_{i}.txt')

# Run Twitter Cache Trace
for i in range(REPEAT_NUM):
    for index in INDEX_LIST:
        for cluster, key_len in zip(TWITTER_TRACE_NUMBER_LIST, TWITTER_KEY_LIST):
            print(f"{index}_{cluster}_{i}")
            os.system(f'../build/LATENCY_BREAKDOWN_{index}_twitter_{key_len}  \
                                --fg={FG_THREADS}           \
                                --runtime={RUNTIME}         \
                                --cluster-number={cluster}  \
                                > ../results/latency_breakdown_{index}_{cluster}_{i}.txt')

# Parse
def extract_breakdown(filename: str) -> List[float]:
    group_traverse = 0.0
    inference = 0.0
    linear_search = 0.0
    range_search = 0.0
    buffer_search = 0.0
    with open(filename, 'r') as f:
        while True:
            line = f.readline()
            if line == "": break
            elif "[micro] group traverse latency:" in line:
                splitted = line.split()
                group_traverse = float(splitted[-1])
            elif "[micro] inference latency:" in line:
                splitted = line.split()
                inference = float(splitted[-1])
            elif "[micro] linear search latency:" in line:
                splitted = line.split()
                linear_search = float(splitted[-1])
            elif "[micro] range search latency:" in line:
                splitted = line.split()
                range_search = float(splitted[-1])
            elif "[micro] buffer search latency:" in line:
                splitted = line.split()
                buffer_search = float(splitted[-1])
    return [group_traverse, inference, linear_search, range_search, buffer_search]

# Export
print("Experiment Results will be exported to ./latency_breakdown_ycsb.csv")
with open('./latency_breakdown_ycsb.csv', 'w') as f:
    wr = csv.writer(f)
    wr.writerow(['Index', 'Dataset', 'Workload', 'Tree Traverse(us)', 'ML Inference(us)', 'Local Search(us)', 'Range Scan(us)', "Buffer Search(us)"])
    for index in INDEX_LIST:
        for dataset in YCSB_DATASET_LIST:
            for workload in YCSB_WORKLOAD_LIST:
                group_traverse_sum = 0.0
                inference_sum = 0.0
                linear_search_sum = 0.0
                range_search_sum = 0.0
                buffer_search_sum = 0.0
                valid_counter = 0
                for i in range(REPEAT_NUM):
                    group_traverse, inference, linear_search, range_search, buffer_search \
                        = extract_breakdown(f"../results/latency_breakdown_{index}_{dataset}_{workload}_{i}.txt")
                    if group_traverse != 0.0:
                        group_traverse_sum += 0 if isnan(group_traverse) else group_traverse
                        inference_sum += 0 if isnan(inference) else inference
                        linear_search_sum += 0 if isnan(linear_search) else linear_search
                        range_search_sum += 0 if isnan(range_search) else range_search
                        buffer_search_sum += 0 if isnan(buffer_search) else buffer_search
                        valid_counter += 1

                group_traverse_avg = group_traverse_sum / valid_counter if valid_counter != 0 else 0
                inference_avg = inference_sum / valid_counter if valid_counter != 0 else 0
                linear_search_avg = linear_search_sum / valid_counter if valid_counter != 0 else 0
                range_search_avg = range_search_sum / valid_counter if valid_counter != 0 else 0
                buffer_search_avg = buffer_search_sum / valid_counter if valid_counter != 0 else 0
                wr.writerow([index, dataset, workload, group_traverse_avg, inference_avg, linear_search_avg, range_search_avg, buffer_search_avg])

print("Experiment Results will be exported to ./latency_breakdown_twitter.csv")
with open('./latency_breakdown_twitter.csv', 'w') as f:
    wr = csv.writer(f)
    wr.writerow(['Index', 'Cluster', 'Tree Traverse(us)', 'ML Inference(us)', 'Local Search(us)', 'Range Scan(us)', "Buffer Search(us)"])
    for index in INDEX_LIST:
        for cluster in TWITTER_TRACE_NUMBER_LIST:
            group_traverse_sum = 0.0
            inference_sum = 0.0
            linear_search_sum = 0.0
            range_search_sum = 0.0
            buffer_search_sum = 0.0
            valid_counter = 0
            for i in range(REPEAT_NUM):
                group_traverse, inference, linear_search, range_search, buffer_search \
                    = extract_breakdown(f"../results/latency_breakdown_{index}_{cluster}_{i}.txt")
                if group_traverse != 0.0:
                    group_traverse_sum += 0 if isnan(group_traverse) else group_traverse
                    inference_sum += 0 if isnan(inference) else inference
                    linear_search_sum += 0 if isnan(linear_search) else linear_search
                    range_search_sum += 0 if isnan(range_search) else range_search
                    buffer_search_sum += 0 if isnan(buffer_search) else buffer_search
                    valid_counter += 1

            group_traverse_avg = group_traverse_sum / valid_counter if valid_counter != 0 else 0
            inference_avg = inference_sum / valid_counter if valid_counter != 0 else 0
            linear_search_avg = linear_search_sum / valid_counter if valid_counter != 0 else 0
            range_search_avg = range_search_sum / valid_counter if valid_counter != 0 else 0
            buffer_search_avg = buffer_search_sum / valid_counter if valid_counter != 0 else 0
            wr.writerow([index, cluster, group_traverse_avg, inference_avg, linear_search_avg, range_search_avg, buffer_search_avg])
        
# Cleanup
os.system(f"make -C ../results")