#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstdlib>

#define COUT_THIS(this) std::cout << this << std::endl;

int main(int argc, char **argv)
{
    // argv[1]: input filename
    // argv[2]: output path
    // argv[3]: number of threads
    char filename[100];
    sprintf(filename, "%s", argv[1]);
    printf("filename: '%s'\n", filename);

    char outputpath[100];
    sprintf(outputpath, "%s", argv[2]);
    printf("outputpath: '%s'\n", outputpath);

    int num_workers = atoi(argv[3]);

    printf("#workers:%d,cluster no.:%s\n", num_workers, filename);

    std::ifstream trace_file(filename);
    std::string trace_line;

    int linecount = 0;
    int idx;
    char split_filename[150];

    std::ofstream files[num_workers];

    // simply copy file
    if (num_workers == 1) {
        sprintf(split_filename, "%s/workload_00", outputpath);

        std::ifstream  src(filename, std::ios::binary);
        std::ofstream  dst(split_filename,   std::ios::binary);

        dst << src.rdbuf();
        return 0;
    }

    // split file into multiple traces
    for (int i = 0; i < num_workers; i++) {
        sprintf(split_filename, "%s/workload_%02d", outputpath, i);
        files[i].open(split_filename);
        COUT_THIS("Open " << split_filename);
    }

    while (getline(trace_file, trace_line))
    {
        idx = linecount % num_workers;
        files[idx] << trace_line << "\n";
        linecount++;
    }

    COUT_THIS("Total line count: " << linecount);
    COUT_THIS("Closing files...")

    for (int i=0; i<num_workers; i++) {
        files[i].close();
    }

    COUT_THIS("Spliting is done !");

    return 0;
}