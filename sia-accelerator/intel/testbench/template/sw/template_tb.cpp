#include <iostream>
#include <string>
#include <unistd.h>
#include <cstdlib>
#include <ctime>
#include <random>
#include <queue>
#include <vector>

#include "opae_svc_wrapper.h"
#include "csr_mgr.h"
#include "afu_json_info.h"

using namespace std;
using namespace opae::fpga::types;
using namespace opae::fpga::bbb::mpf::types;

#define TEST_ITER_NUM 5
#define SEQUENCE_TEST_ITER_NUM 3
#define SEQUENCE_LEN 30
typedef int16_t fpga_int;

const int FIFO_DEPTH = 6;

inline void assert_test(int test_num, fpga_int answer, fpga_int result){
    if(answer != result) {
        cout << "Test " << test_num << " failed: FPGA returned (" << result << "), not (" << answer << ")" << endl;
        exit(-1);
    }
}

inline void init_buffer(volatile fpga_int *buffer, int size) {
    for(int i=0; i<size; i++){
        buffer[i] = 0;
    }
}

int main(int argc, char **argv){
    OPAE_SVC_WRAPPER fpga(AFU_ACCEL_UUID);
    if(!fpga.isOk()) return -1;
    CSR_MGR csrs(fpga);

    // Random Integer Generator
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<fpga_int> pe_addr_gen(1, 6);
    uniform_int_distribution<fpga_int> value_gen(0, 32767);
    uniform_int_distribution<fpga_int> true_false(0, 1);
    uniform_int_distribution<fpga_int> small_value_gen(1, 5);

    // Allocate shared memory space
    auto input_buf_handle = fpga.allocBuffer(getpagesize());
    auto input_buf = reinterpret_cast<volatile fpga_int*>(input_buf_handle->c_type());
    if(!input_buf) return -1;

    auto output_buf_handle = fpga.allocBuffer(getpagesize());
    auto output_buf = reinterpret_cast<volatile fpga_int*>(output_buf_handle->c_type());
    if(!output_buf) return -1;

    /********************************************
     *  CSR Mapping                             *
     *  0: Start Signal                         *   -> [x]: start signal
     *  1: Input Buffer Address                 *   -> [0]: data_from_bus   (16bit)
     *                                          *      [1]: addr_from_bus   (3bit + 13bit zeros)
     *                                          *      [2]: valid_from_bus  (1bit + 15bit zeros)
     *                                          *      [3]: src_addr_in     (3bit + 13bit zeros)
     *                                          *      [4]: src_rq_in       (1bit + 15bit zeros)
     *  2: Output Buffer Address                *   -> [0]: end of operation flag (1bit + 15bit zeros)
     *                                          *      [1]: src_data_w      (16bit)
     *                                          *      [2]: src_valid_r     (1bit + 15bit zeros)
     *                                          *      [3]: rd_buffer_full  (8bit + 7bit zeros)
     *  3: Reset Signal                         *   -> [x]: reset signal
     ********************************************/
    // Tell FPGA shared memory addresses
    csrs.writeCSR(1, intptr_t(input_buf));
    csrs.writeCSR(2, intptr_t(output_buf));

    cout << "Use PE_ID == 0 throughout all tests" << endl;

    // Test 1: bus_read gets data from other PEs through bus. Then PE reads ths data.
    cout << "Test 1: bus_read gets data from other PEs through bus. Then PE reads ths data." << endl;
    for(int i=0; i<TEST_ITER_NUM; i++){
        queue<fpga_int> q;
        fpga_int bus_addr = pe_addr_gen(gen);
        int iter_num = small_value_gen(gen);

        cout << "Enqueue from PE " << bus_addr << " will be repeated " << iter_num << " times" << endl;
        for(int j=0; j<iter_num; j++){
            fpga_int bus_data = value_gen(gen);
            init_buffer(input_buf, 5);
            init_buffer(output_buf, 4);

            input_buf[0] = bus_data;
            input_buf[1] = bus_addr;
            input_buf[2] = 1;
            q.push(bus_data);
            cout << "Enqueue value " << bus_data << " from PE " << bus_addr << endl;

            csrs.writeCSR(0, 1);
            while(0 == output_buf[0]) usleep(1);
        }

        cout << "Dequeue from FIFO " << bus_addr << " will be repeated " << iter_num << " times" << endl;
        for(int j=0; j<iter_num; j++){
            init_buffer(input_buf, 5);
            init_buffer(output_buf, 4);

            input_buf[3] = bus_addr;
            input_buf[4] = 1;
            fpga_int answer = q.front();
            q.pop();

            csrs.writeCSR(0, 1);
            while(0 == output_buf[0]) usleep(1);

            assert_test(1, 1, output_buf[2]);
            assert_test(1, answer, output_buf[1]);
            cout << "Dequeue value " << output_buf[1] << " from FIFO " << bus_addr << endl;
        }
        cout << "========== Reset bus_read ==========" << endl;
        csrs.writeCSR(3, 1);
    }

    // Test 2: Select one PE address, and enqueue data until correspond FIFO is full. Then dequeue data until the FIFO is empty.
    cout << "Test 2: Select one PE address, and enqueue data until correspond FIFO is full." << endl;
    cout << "Then dequeue data until the FIFO is empty." << endl;
    for(int i=0; i<TEST_ITER_NUM; i++){
        queue<fpga_int> q;
        fpga_int bus_addr = pe_addr_gen(gen);
        cout << "Enqueue from PE " << bus_addr << " will be repeated until FIFO is full" << endl;
        for(int j=0; j<FIFO_DEPTH; j++){
            fpga_int bus_data = value_gen(gen);
            init_buffer(input_buf, 5);
            init_buffer(output_buf, 4);

            input_buf[0] = bus_data;
            input_buf[1] = bus_addr;
            input_buf[2] = 1;
            q.push(bus_data);
            cout << "Enqueue value " << bus_data << " from PE " << bus_addr << endl;

            csrs.writeCSR(0, 1);
            while(0 == output_buf[0]) usleep(1);
        }

        cout << "Check whether if FIFO is full" << endl;
        assert_test(2, 1 << bus_addr, output_buf[3]);

        cout << "Dequeue from FIFO " << bus_addr << " will be repeated " << FIFO_DEPTH << " times" << endl;
        for(int j=0; j<FIFO_DEPTH; j++){
            init_buffer(input_buf, 5);
            init_buffer(output_buf, 4);

            input_buf[3] = bus_addr;
            input_buf[4] = 1;
            fpga_int answer = q.front();
            q.pop();

            csrs.writeCSR(0, 1);
            while(0 == output_buf[0]) usleep(1);

            assert_test(2, 1, output_buf[2]);
            assert_test(2, answer, output_buf[1]);
            cout << "Dequeue value " << output_buf[1] << " from FIFO " << bus_addr << endl;
        }
        cout << "========== Reset bus_read ==========" << endl;
        csrs.writeCSR(3, 1);
    }

    // Test 3: Generate random bus data and read request sequence.
    cout << "Test 3: Generate random bus data and read request sequence." << endl;
    for(int i=0; i<SEQUENCE_TEST_ITER_NUM; i++){
        // Initialize Queue
        vector<queue<fpga_int>> vector_of_q;
        for(int j=0; j<7; j++){
            vector_of_q.push_back(queue<fpga_int>());
        }

        for(int j=0; j<SEQUENCE_LEN; j++){
            fpga_int bus_addr = pe_addr_gen(gen);
            if(true_false(gen) && (vector_of_q[bus_addr].size() != FIFO_DEPTH)){
                // Enqueue
                fpga_int bus_data = value_gen(gen);
                init_buffer(input_buf, 5);
                init_buffer(output_buf, 4);

                input_buf[0] = bus_data;
                input_buf[1] = bus_addr;
                input_buf[2] = 1;
                vector_of_q[bus_addr].push(bus_data);
                cout << "Enqueue value " << bus_data << " from PE " << bus_addr << endl;

                csrs.writeCSR(0, 1);
                while(0 == output_buf[0]) usleep(1);

                if(vector_of_q[bus_addr].size() == FIFO_DEPTH) {
                    assert_test(3, 1 << bus_addr, output_buf[3]);
                    cout << "FIFO is full" << endl;
                }
            } else {
                // Dequeue
                init_buffer(input_buf, 5);
                init_buffer(output_buf, 4);

                input_buf[3] = bus_addr;
                input_buf[4] = 1;
                if(vector_of_q[bus_addr].size() == 0) {
                    csrs.writeCSR(0, 1);
                    while(0 == output_buf[0]) usleep(1);

                    assert_test(3, 0, output_buf[2]);
                    cout << "FIFO " << bus_addr << " is empty" << endl;
                } else {
                    fpga_int answer = vector_of_q[bus_addr].front();
                    vector_of_q[bus_addr].pop();

                    csrs.writeCSR(0, 1);
                    while(0 == output_buf[0]) usleep(1);

                    assert_test(3, 1, output_buf[2]);
                    assert_test(3, answer, output_buf[1]);
                    cout << "Dequeue value " << output_buf[1] << " from FIFO " << bus_addr << endl;
                }
            }
        }

        cout << "========== Reset bus_read ==========" << endl;
        csrs.writeCSR(3, 1);
    }

    return 0;
}