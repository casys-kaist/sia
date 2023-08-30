#include <iostream>
#include <string>
#include <unistd.h>
#include <cstdlib>
#include <ctime>
#include <random>
#include <queue>
#include <vector>
#include <limits>
#include <math.h>

#include "opae_svc_wrapper.h"
#include "csr_mgr.h"
#include "afu_json_info.h"

using namespace std;
using namespace opae::fpga::types;
using namespace opae::fpga::bbb::mpf::types;

#define TEST_ITER_NUM 5
typedef int32_t fpga_int;
typedef float fpga_float;

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

union float_to_binary {
    float f;
    uint32_t i;
};  

inline fpga_int convert_float_to_bin(fpga_float input) {    
    union float_to_binary converter;
    converter.f = input;
    return converter.i;
}

inline fpga_float convert_bin_to_float(fpga_int input) {    
    union float_to_binary converter;
    converter.i = input;
    return converter.f;
}

int main(int argc, char **argv){
    OPAE_SVC_WRAPPER fpga(AFU_ACCEL_UUID);
    if(!fpga.isOk()) return -1;
    CSR_MGR csrs(fpga);

    // Random Integer Generator
    //random_device rd;
    //mt19937 gen(rd());
    //uniform_real_distribution<fpga_float> float_gen(-10000.0f, 10000.0f);
    //uniform_int_distribution<fpga_int> true_false(0, 1);

    // Allocate shared memory space
    auto input_u_buf_handle = fpga.allocBuffer(getpagesize());
    auto input_u_buf = reinterpret_cast<volatile fpga_int*>(input_u_buf_handle->c_type());
    if(!input_u_buf) return -1;
    
    auto input_a_buf_handle = fpga.allocBuffer(getpagesize());
    auto input_a_buf = reinterpret_cast<volatile fpga_int*>(input_a_buf_handle->c_type());
    if(!input_a_buf) return -1;

    auto output_buf_handle = fpga.allocBuffer(getpagesize());
    auto output_buf = reinterpret_cast<volatile fpga_int*>(output_buf_handle->c_type());
    if(!output_buf) return -1;

    /********************************************
     *  CSR Mapping                             *
     *  0: Start Signal                         *   -> [x]: length of the input vector (x 8)
     *  1: Input U Buffer Address               *   -> [i]: i-th input vector element  (32bit - 1bit sign, 8bit exp, 23bit frac)
     *  2: Input A Buffer Address               *   -> [i]: i-th input vector element  (32bit - 1bit sign, 8bit exp, 23bit frac)
     *  3: Output Buffer Address                *   -> [i]: i-th output vector element (32bit - 1bit sign, 8bit exp, 23bit frac)
     *  4: Input Gamma Value                    *   -> [0]: Input gamma value          (32bit - 1bit sign, 8bit exp, 23bit frac)
     *  5: Reset Signal                         *   -> [x]: reset signal
     ********************************************/
    fpga_float input_gamma = -0.0013118337;

    // Tell FPGA shared memory addresses
    csrs.writeCSR(1, intptr_t(input_u_buf));
    csrs.writeCSR(2, intptr_t(input_a_buf));
    csrs.writeCSR(4, convert_float_to_bin(input_gamma));
    csrs.writeCSR(3, intptr_t(output_buf));

    // Test 1: Hardcoded Input Vector
    cout << "\nTest 1: Hardcoded Input Vector" << endl;
    {
        // Define inputs and expected outputs
        fpga_float input_a[24] = {2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0,
                                  1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0,
                                  9.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0};
        
        fpga_float input_u[24] = {28.645824, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0,
                                  1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0,
                                  9.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0};

        // Initialize io buffer
        init_buffer(input_u_buf, 32);
        init_buffer(input_a_buf, 32);
        init_buffer(output_buf, 32);

        // Fill buffer
        for (int i=0; i<24; i++) {
            input_u_buf[i] = convert_float_to_bin(input_u[i]);
            input_a_buf[i] = convert_float_to_bin(input_a[i]);
        }

        // Trigger FPGA
        csrs.writeCSR(0, 3);
        
        // Wait until end
        while(0 == output_buf[0]) usleep(1);

        // Print results
        usleep(100);
        cout << "FPGA-side Output: ";
        for (int i=0; i<24; i++){
            cout << convert_bin_to_float(output_buf[i]) << " ";
        }
        cout << endl;

        // Reset FPGA
        csrs.writeCSR(5, 1);
    }

    cout << "\nAll Test Done" << endl;

    return 0;
}