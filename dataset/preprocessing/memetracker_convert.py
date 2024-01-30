import sys
import random

if len(sys.argv) < 2:
    print(f"Usage: {sys.argv[0]} [output_dir]")
    exit(0)

output_dir = sys.argv[1]

file_list = [
    "../raw_dataset/quotes_2009-04.txt",
    "../raw_dataset/quotes_2009-03.txt",
    "../raw_dataset/quotes_2009-02.txt",
    "../raw_dataset/quotes_2009-01.txt",
    "../raw_dataset/quotes_2008-12.txt"
]

NUM_THREADS=16
INIT_KEYS=10_000_000
PER_THREAD_KEYS=40_000_000
KEY_LENGTH=128

for WORKLOAD in ["D", "E"]:
    current_state="load"
    counter = 0
    current_thread = 0

    current_file = open(f"{output_dir}/Workload{WORKLOAD}/workload_{WORKLOAD}_load", "w")
    worker_files = [open(f"{output_dir}/Workload{WORKLOAD}/workload_{WORKLOAD}_worker_{i}", "w") for i in range(NUM_THREADS)]

    key_buffer = list() # Used for workload D
    inserted_keys = list() # Used for workload E

    def generate_op(workload, current_key):
        global key_buffer
        if workload == "D":
            random_num = random.random()
            if random_num < 0.9:
                if len(key_buffer) == 0:
                    return "r", current_key
                else:
                    random_int = random.randrange(0, len(key_buffer))
                    return "r", key_buffer[random_int]
            else:
                if len(key_buffer) == 10:
                    key_buffer.pop(0)
                key_buffer.append(current_key)
                return "i", current_key
        elif workload == "E":
            random_num = random.random()
            if random_num < 0.9:
                random_int = random.randrange(0, len(inserted_keys))
                return "s", inserted_keys[random_int]
            else:
                inserted_keys.append(current_key)
                return "i", current_key

    for filename in file_list:
        with open(filename, "r") as f:
            while True:
                line = f.readline()
                if not line:
                    break
                else:
                    line = line.split()
                    if len(line) < 2:
                        continue
                    if (line[0] != "L") and (line[0] != "P"):
                        continue
                    key = line[1]
                    key = key + "/" * (KEY_LENGTH - len(key))

                    if current_state == "load":
                        counter += 1
                        if counter == INIT_KEYS + 1:
                            counter = 0
                            current_state = "thread"
                            current_file.close()
                        else:
                            inserted_keys.append(key[:])
                            current_file.write(f"{key[:]}\n")

                    elif current_state == "thread":
                        if current_thread == NUM_THREADS:
                            current_thread = 0
                            counter += 1
                            if counter >= PER_THREAD_KEYS - 1:
                                break
                        op, query_key = generate_op(WORKLOAD, key)
                        worker_files[current_thread].write(f"{op} {query_key[:]}\n")
                        current_thread += 1

    for i in range(NUM_THREADS):
        worker_files[i].close()