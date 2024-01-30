#include <iostream>
#include <fstream>
#include <string>

using std::string;

/*
 Reformat Twitter trace for xindex
 
 Twitter trace form
 format) timestamp,anonymized_key,key_size,value_size,client_id,operation,TTL
 ex) 0,Nz_ztinzCiiCmiKCzizKQi,22,30,1,add,7200

 YCSB trace form
 format) operation_id key
 ex) i user4733205558962263268

 <Twitter operation mapping>
 twitter operations - xindex operation
 - get/gets: get
 - set/replace/cas/add/append/prepend/incr/decr: put
 - delete: remove

*/

bool isNumber(const string& str)
{
    for (char const &c: str) {
        if (!std::isdigit(c)) return false;
    }
    return true;
}

// argv[1]: input filename
// argv[2]: output filename

int main(int argc, char **argv)
{    
    char filename[100];
    char new_filename[100];
    sprintf(filename, "%s", argv[1]);
    sprintf(new_filename, "%s", argv[2]);

    std::ifstream trace_file(filename);
    std::string trace_line;
    std::ofstream new_trace_file;

    if (trace_file.fail())
    {
        std::cout << filename << " does not exist." << std::endl;
        return 1;
    }

    // Create/Open new trace file
    new_trace_file.open(new_filename);
    std::cout << "Opened " << new_filename << std::endl;

    // Read original trace file
    while (getline(trace_file, trace_line))
    {
        char ops;
        std::string key;
        size_t idx = 0;
        size_t pos = 0;
        std::string token;
        std::string delimiter = ",";
        std::string line = trace_line;

        // Parse the line and Extract key and operaion type from it
        while((pos = trace_line.find(delimiter)) != std::string::npos) 
        {
            token = trace_line.substr(0, pos);
            trace_line.erase(0, pos + delimiter.length());
            
            if (idx == 1) // key
                key = token;
            if (idx == 2 && !isNumber(token)) {
                key.append("," + token);
                continue;
            }
            if (idx == 5) { // operation
                if (!token.compare("get") || !token.compare("gets")) {
                    ops = 'g';
                } else if (!token.compare("set") || !token.compare("replace") || !token.compare("cas") || !token.compare("add") || !token.compare("append") || !token.compare("prepend") || !token.compare("incr") || !token.compare("decr")) {
                    ops = 'p';
                } else if (!token.compare("delete")) {
                    ops = 'd';
                } else {
                    std::cout << "Strange operation.. (operation:" << token << ")" << std::endl;
                    std::cout << "current line:" <<std::endl << line << std::endl;
                    std::cout << "key: " << key << std::endl;
                    return 1;
                }
            }
            idx++;
        }
        // Write new trace line to new trace file
        new_trace_file << ops << " " << key << std::endl;
    }

    new_trace_file.close();
    return 0;
}

