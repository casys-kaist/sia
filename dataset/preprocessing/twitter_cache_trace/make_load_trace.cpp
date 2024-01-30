#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <set>
#include <string>

using namespace std;

/* Make load trace file */
// Contain only key
// argv[1]: input filename
// argv[2]: output filename
// argv[3]: table size (# of keys)

int main(int argc, char **argv)
{
	char filename[50];
	char load_trace_filename[50];

	sprintf(filename, "%s", argv[1]);
	sprintf(load_trace_filename, "%s", argv[2]);
	int num_trace = atoi(argv[3]);
	

	std::cout << "filename: " << filename <<std::endl;

	std::set<string> key_set;
	std::pair<std::set<string>::iterator, bool> ret;
	std::ifstream tracefile(filename);
	std::string line;
	char op;
	char key[100];
	size_t tablesize = 0;

	// Open load trace file
	std::ofstream load_trace_file;
	load_trace_file.open(load_trace_filename);

	for (int i = 0; i < num_trace; i++) {
		std::getline(tracefile, line);

		sscanf(line.c_str(), "%c %s", &op, key);

		ret = key_set.insert(key);
		if (ret.second) {
			tablesize++;
			load_trace_file << key << std::endl;
		}
	}

	load_trace_file.close();

	printf("Successfully made load trace file with %ld keys!\n", tablesize);
	return 0;
}
