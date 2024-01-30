import sys
import os

if len(sys.argv) < 4:
    print(f"Usage: {sys.argv[0]} [cluster_number] [input_filepath] [output_dir]")
    exit(0)

cluster_number = sys.argv[1]
input_filename = sys.argv[2]
output_dir = sys.argv[3]

TABLE_SIZE = 10_000_000
THREAD_NUMBER = 16

# 0. Make
os.system(f'make -C twitter_cache_trace')

# 1. reformatting
os.system(f'./twitter_cache_trace/reformat {input_filename} ./reformatted_{cluster_number}')

# 2. make_load_trace
os.system(f'./twitter_cache_trace/make_load_trace ./reformatted_{cluster_number} {output_dir}/{cluster_number}/load{cluster_number} {TABLE_SIZE}')

# 3. split_workload_trace
os.system(f'./twitter_cache_Trace/split_workload_trace ./reformatted_{cluster_number} {output_dir}/{cluster_number} {THREAD_NUMBER}')

# 4. cleanup
os.system(f'rm -f ./reformatted_{cluster_number}')