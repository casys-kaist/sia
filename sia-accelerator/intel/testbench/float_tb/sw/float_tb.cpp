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

int main(int argc, char **argv){
    OPAE_SVC_WRAPPER fpga(AFU_ACCEL_UUID);
    if(!fpga.isOk()) return -1;
    CSR_MGR csrs(fpga);

    // Random Integer Generator
    random_device rd;
    mt19937 gen(rd());
    uniform_real_distribution<fpga_float> float_gen(-10000.0f, 10000.0f);
    uniform_int_distribution<fpga_int> true_false(0, 1);

    // Allocate shared memory space
    auto input_buf_handle = fpga.allocBuffer(getpagesize());
    auto input_buf = reinterpret_cast<volatile fpga_int*>(input_buf_handle->c_type());
    if(!input_buf) return -1;

    auto output_buf_handle = fpga.allocBuffer(getpagesize());
    auto output_buf = reinterpret_cast<volatile fpga_int*>(output_buf_handle->c_type());
    if(!output_buf) return -1;

    /********************************************
     *  CSR Mapping                             *
     *  0: Start Signal                         *   -> [x]: operation number (0: add, 1: mult, 2: div, 3: sqrt)
     *  1: Input Buffer Address                 *   -> [0]: input_a   (32bit - 1bit sign, 8bit exp, 23bit frac)
     *                                          *      [1]: input_b   (32bit - 1bit sign, 8bit exp, 23bit frac)
     *  2: Output Buffer Address                *   -> [0]: end of operation flag (1bit + 31bit zeros)
     *                                          *      [1]: output    (32bit - 1bit sign, 8bit exp, 23bit frac)
     *  3: Reset Signal                         *   -> [x]: reset signal
     ********************************************/
    // Tell FPGA shared memory addresses
    csrs.writeCSR(1, intptr_t(input_buf));
    csrs.writeCSR(2, intptr_t(output_buf));

    auto do_test = [&](fpga_float input_a, fpga_float input_b, int operation) -> fpga_float {
        union float_to_binary convert_input_a;
        union float_to_binary convert_input_b;
        union float_to_binary convert_input_o;

        convert_input_a.f = input_a;
        convert_input_b.f = input_b;

        init_buffer(input_buf, 2);
        init_buffer(output_buf, 2);

        input_buf[0] = convert_input_a.i;
        input_buf[1] = convert_input_b.i;

        csrs.writeCSR(0, operation);
        while(0 == output_buf[0]) usleep(1);

        convert_input_o.i = output_buf[1];

        return convert_input_o.f;
    };

    // Test 1-1: Test Floating Point Adder
    cout << "\nTest 1-1: Test Floating Point Adder" << endl;
    for(int i=0; i<TEST_ITER_NUM; i++){
        float input_a = float_gen(gen);
        float input_b = float_gen(gen);
        float input_o = do_test(input_a, input_b, 0);

        cout << input_a << " + " << input_b << endl;
        cout << "FPGA-side answer " << input_o << " CPU-side answer: " << (input_a + input_b) << endl;

        csrs.writeCSR(3, 1);
    }

    // Test 1-2: Test Floating Point Adder (Special Cases)
    cout << "\nTest 1-2: Test Floating Point Adder (Special Cases)" << endl;
    for(int i=0; i<TEST_ITER_NUM; i++){
        float input_a = float_gen(gen);
        float input_b = float_gen(gen);
        if(true_false(gen)) input_a = 0.0f;
        else                input_b = 0.0f;

        float input_o = do_test(input_a, input_b, 0);

        cout << input_a << " + " << input_b << endl;
        cout << "FPGA-side answer " << input_o << " CPU-side answer: " << (input_a + input_b) << endl;

        csrs.writeCSR(3, 1);
    }

    // Test 2-1: Test Floating Point Multiplier
    cout << "\nTest 2-1: Test Floating Point Multiplier" << endl;
    for(int i=0; i<TEST_ITER_NUM; i++){
        float input_a = float_gen(gen);
        float input_b = float_gen(gen);
        float input_o = do_test(input_a, input_b, 1);

        cout << input_a << " * " << input_b << endl;
        cout << "FPGA-side answer " << input_o << " CPU-side answer: " << (input_a * input_b) << endl;

        csrs.writeCSR(3, 1);
    }

    // Test 2-2: Test Floating Point Multiplier (Special Cases)
    cout << "\nTest 2-2: Test Floating Point Multiplier (Special Cases)" << endl;
    for(int i=0; i<TEST_ITER_NUM; i++){
        float input_a = float_gen(gen);
        float input_b = float_gen(gen);
        if(true_false(gen)) input_a = 0.0f;
        else                input_b = 0.0f;

        float input_o = do_test(input_a, input_b, 1);

        cout << input_a << " * " << input_b << endl;
        cout << "FPGA-side answer " << input_o << " CPU-side answer: " << (input_a * input_b) << endl;

        csrs.writeCSR(3, 1);
    }

    // Test 3-1: Test Floating Point TwoDivider
    cout << "\nTest 3-1: Test Floating Point TwoDivider" << endl;
    for(int i=0; i<TEST_ITER_NUM; i++){
        float input_a = float_gen(gen);
        float input_b = 0.0f;
        float input_o = do_test(input_a, input_b, 2);

        cout << -2 << " / " << input_a << endl;
        cout << "FPGA-side answer " << input_o << " CPU-side answer: " << (-2 / input_a) << endl;

        csrs.writeCSR(3, 1);
    }

    // Test 3-2: Test Floating Point TwoDivider (Special Case)
    cout << "\nTest 3-2: Test Floating Point TwoDivider (Special Case)" << endl;
    for(int i=0; i<TEST_ITER_NUM; i++){
        float input_a = 0.0f;
        float input_b = 0.0f;
        float input_o = do_test(input_a, input_b, 2);

        cout << -2 << " / " << input_a << endl;
        cout << "FPGA-side answer " << input_o << " CPU-side answer: " << "NAN" << endl;

        csrs.writeCSR(3, 1);
    }

    // Test 4-1: Test Floating Point Sqrt
    cout << "\nTest 4-1: Test Floating Point Sqrt" << endl;
    for(int i=0; i<TEST_ITER_NUM; i++){
        float input_a = float_gen(gen);
        float input_b = 0.0f;
        if (input_a < 0) input_a *= -1;
        float input_o = do_test(input_a, input_b, 3);

        cout << "Sqrt " << input_a << endl;
        cout << "FPGA-side answer " << input_o << " CPU-side answer: " << sqrtf(input_a) << endl;

        csrs.writeCSR(3, 1);
    }
    
    // Test 4-1: Test Floating Point Sqrt (Special Cases)
    cout << "\nTest 4-1: Test Floating Point Sqrt (Special Cases)" << endl;
    for(int i=0; i<TEST_ITER_NUM; i++){
        float input_a = (float)(i * i);
        float input_b = 0.0f;
        if (input_a < 0) input_a *= -1;
        float input_o = do_test(input_a, input_b, 3);

        cout << "Sqrt " << input_a << endl;
        cout << "FPGA-side answer " << input_o << " CPU-side answer: " << sqrtf(input_a) << endl;

        csrs.writeCSR(3, 1);
    }

    cout << "\nAll Test Done" << endl;

    return 0;
}