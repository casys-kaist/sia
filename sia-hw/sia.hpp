#include <vector>
#include <cassert>

#include "mkl.h"
#include "mkl_lapacke.h"

#include "opae_svc_wrapper.h"
#include "csr_mgr.h"
#include "afu_json_info.h"

#include "../test_config.h"

using namespace std;
using namespace opae::fpga::types;
using namespace opae::fpga::bbb::mpf::types;

typedef int32_t fpga_int;
typedef float fpga_float;

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

namespace sindex {

OPAE_SVC_WRAPPER fpga;
CSR_MGR csrs;
bool init_flag = false;
double *input_buf;
double *output_buf;

inline void init_accelerator() {
    fpga = OPAE_SVC_WRAPPER(AFU_ACCEL_UUID);
    if(!fpga.isOk()) { 
        COUT_N_EXIT("FPGA is not available.");
    }
    csrs = CSR_MGR(fpga);

    // Allocate shared memory space
    auto input_buf_handle = fpga.allocBuffer(getpagesize());
    input_buf = reinterpret_cast<volatile fpga_int*>(input_buf_handle->c_type());
    if(!input_buf) return -1;

    auto output_buf_handle = fpga.allocBuffer(getpagesize());
    output_buf = reinterpret_cast<volatile fpga_int*>(output_buf_handle->c_type());
    if(!output_buf) return -1;

    init_flag = true;
    return;
}

inline void incremental_training(
    float *delta_a, int delta_m,
    float *delta_b, int delta_n,
    float *inserted_a, int inserted_m,
    float *inserted_b, int inserted_n,
    float **cached_matrix_ptr,
    float *model_weights
) {
    if (!init_flag) { 
        COUT_N_EXIT("FPGA is not initialized.");
    }

    // Check shared memory space
    assert(input_buf);
    assert(output_buf);
    assert((*cached_matrix_ptr)); // There must be cached matrix allocated
    float *cached_matrix = *cached_matrix_ptr;

    // Tell FPGA shared memory addresses
    csrs.writeCSR(1, intptr_t(input_buf));
    csrs.writeCSR(2, intptr_t(output_buf));
    csrs.writeCSR(3, intptr_t(cached_matrix));

    // Write a matrix to the fpga
    for (int i=0; i<delta_n; i++) {
        int current_column = i;
        for (int j=0; j<delta_m; j+=8) {
            int current_row_start = j;
            ((char *)input_buf)[0] = true; // write_req_valid
            ((char *)input_buf)[1] = true; // write_req_bits
            ((char *)input_buf)[2] = (current_column >> 5) & 0x1;
            ((char *)input_buf)[3] = (current_column >> 4) & 0x1;
            ((char *)input_buf)[4] = (current_column >> 3) & 0x1;
            ((char *)input_buf)[5] = (current_column >> 2) & 0x1;
            ((char *)input_buf)[6] = (current_column >> 1) & 0x1;
            ((char *)input_buf)[7] = (current_column >> 0) & 0x1; // write_column
            ((char *)input_buf)[8] = (current_row_start >> 6) & 0x1;
            ((char *)input_buf)[9] = (current_row_start >> 5) & 0x1;
            ((char *)input_buf)[10] = (current_row_start >> 4) & 0x1;
            ((char *)input_buf)[11] = (current_row_start >> 3) & 0x1;
            ((char *)input_buf)[12] = (current_row_start >> 2) & 0x1;
            ((char *)input_buf)[13] = (current_row_start >> 1) & 0x1;
            ((char *)input_buf)[14] = (current_row_start >> 0) & 0x1; // write_row
            (fpga_float *)((char *)input_buf)[15] = delta_a[(current_row_start + 0) * n + current_column];
            (fpga_float *)((char *)input_buf)[47] = delta_a[(current_row_start + 1) * n + current_column];
            (fpga_float *)((char *)input_buf)[79] = delta_a[(current_row_start + 2) * n + current_column];
            (fpga_float *)((char *)input_buf)[126] = delta_a[(current_row_start + 3) * n + current_column];
            (fpga_float *)((char *)input_buf)[173] = delta_a[(current_row_start + 4) * n + current_column];
            (fpga_float *)((char *)input_buf)[220] = delta_a[(current_row_start + 5) * n + current_column];
            (fpga_float *)((char *)input_buf)[267] = delta_a[(current_row_start + 6) * n + current_column];
            (fpga_float *)((char *)input_buf)[314] = delta_a[(current_row_start + 7) * n + current_column];
            ((char *)input_buf)[361] = false; // read_req
            ((char *)input_buf)[362] = false;
            ((char *)input_buf)[363] = false;
            ((char *)input_buf)[364] = false;
            ((char *)input_buf)[365] = false;
            ((char *)input_buf)[366] = false; 
            ((char *)input_buf)[367] = false; // read_column
            ((char *)input_buf)[368] = false;
            ((char *)input_buf)[369] = false;
            ((char *)input_buf)[370] = false;
            ((char *)input_buf)[371] = false;
            ((char *)input_buf)[372] = false;
            ((char *)input_buf)[373] = false;
            ((char *)input_buf)[374] = false; // read_row
            ((char *)input_buf)[375] = false; // op_valid
            ((char *)input_buf)[376] = false;
            ((char *)input_buf)[377] = false; // op_bits
            ((char *)input_buf)[378] = false; // used_pus_valid
            ((char *)input_buf)[379] = false; // used_pus_bits
            ((char *)input_buf)[380] = false;
            ((char *)input_buf)[381] = false;
            ((char *)input_buf)[382] = false;
            ((char *)input_buf)[383] = false;
            ((char *)input_buf)[384] = false; // length_bits_0
            ((char *)input_buf)[385] = false;
            ((char *)input_buf)[386] = false;
            ((char *)input_buf)[387] = false;
            ((char *)input_buf)[388] = false;
            ((char *)input_buf)[389] = false; // length_bits_1

            init_buffer(output_buf, 32);
            while (output_buf[0] == false) {;}
            init_buffer(input_buf, 32);
        }
    }

    // Write b vector to the fpga
    for (int i=0; i<delta_m; i+=8) {
        int current_row_start = i;
        ((char *)input_buf)[0] = true; // write_req_valid
        ((char *)input_buf)[1] = false; // write_req_bits
        ((char *)input_buf)[2] = false;
        ((char *)input_buf)[3] = false;
        ((char *)input_buf)[4] = false;
        ((char *)input_buf)[5] = false;
        ((char *)input_buf)[6] = false;
        ((char *)input_buf)[7] = false; // write_column
        ((char *)input_buf)[8] = (current_row_start >> 6) & 0x1;
        ((char *)input_buf)[9] = (current_row_start >> 5) & 0x1;
        ((char *)input_buf)[10] = (current_row_start >> 4) & 0x1;
        ((char *)input_buf)[11] = (current_row_start >> 3) & 0x1;
        ((char *)input_buf)[12] = (current_row_start >> 2) & 0x1;
        ((char *)input_buf)[13] = (current_row_start >> 1) & 0x1;
        ((char *)input_buf)[14] = (current_row_start >> 0) & 0x1; // write_row
        (fpga_float *)((char *)input_buf)[15] = delta_b[(current_row_start + 0)];
        (fpga_float *)((char *)input_buf)[47] = delta_b[(current_row_start + 1)];
        (fpga_float *)((char *)input_buf)[79] = delta_b[(current_row_start + 2)];
        (fpga_float *)((char *)input_buf)[126] = delta_b[(current_row_start + 3)];
        (fpga_float *)((char *)input_buf)[173] = delta_b[(current_row_start + 4)];
        (fpga_float *)((char *)input_buf)[220] = delta_b[(current_row_start + 5)];
        (fpga_float *)((char *)input_buf)[267] = delta_b[(current_row_start + 6)];
        (fpga_float *)((char *)input_buf)[314] = delta_b[(current_row_start + 7)];
        ((char *)input_buf)[361] = false; // read_req
        ((char *)input_buf)[362] = false;
        ((char *)input_buf)[363] = false;
        ((char *)input_buf)[364] = false;
        ((char *)input_buf)[365] = false;
        ((char *)input_buf)[366] = false; 
        ((char *)input_buf)[367] = false; // read_column
        ((char *)input_buf)[368] = false;
        ((char *)input_buf)[369] = false;
        ((char *)input_buf)[370] = false;
        ((char *)input_buf)[371] = false;
        ((char *)input_buf)[372] = false;
        ((char *)input_buf)[373] = false;
        ((char *)input_buf)[374] = false; // read_row
        ((char *)input_buf)[375] = false; // op_valid
        ((char *)input_buf)[376] = false;
        ((char *)input_buf)[377] = false; // op_bits
        ((char *)input_buf)[378] = false; // used_pus_valid
        ((char *)input_buf)[379] = false; // used_pus_bits
        ((char *)input_buf)[380] = false;
        ((char *)input_buf)[381] = false;
        ((char *)input_buf)[382] = false;
        ((char *)input_buf)[383] = false;
        ((char *)input_buf)[384] = false; // length_bits_0
        ((char *)input_buf)[385] = false;
        ((char *)input_buf)[386] = false;
        ((char *)input_buf)[387] = false;
        ((char *)input_buf)[388] = false;
        ((char *)input_buf)[389] = false; // length_bits_1

        init_buffer(output_buf, 32);
        while (output_buf[0] == false) {;}
        init_buffer(input_buf, 32);
    }

    // Trigger incremental training
    for (int k=0; k<=374; k++) ((char *)input_buf)[k] = false;
    ((char *)input_buf)[375] = true; // op_valid
    ((char *)input_buf)[376] = true;
    ((char *)input_buf)[377] = true; // op_bits
    ((char *)input_buf)[378] = true; // used_pus_valid
    ((char *)input_buf)[379] = true; // used_pus_bits
    ((char *)input_buf)[380] = (m >> 4) & 0x1;
    ((char *)input_buf)[381] = (m >> 3) & 0x1;
    ((char *)input_buf)[382] = (m >> 2) & 0x1;
    ((char *)input_buf)[383] = (m >> 1) & 0x1;
    ((char *)input_buf)[384] = (m >> 0) & 0x1; // length_bits_0
    ((char *)input_buf)[385] = (m >> 4) & 0x1;
    ((char *)input_buf)[386] = (m >> 3) & 0x1;
    ((char *)input_buf)[387] = (m >> 2) & 0x1;
    ((char *)input_buf)[388] = (m >> 1) & 0x1;
    ((char *)input_buf)[389] = (m >> 0) & 0x1; // length_bits_1

    // Sync
    init_buffer(output_buf, 32);
    while (output_buf[0] == false) {;}
    init_buffer(input_buf, 32);

    // "Trigger" read cached_matrix from the fpga
    // !! Do not access cached_matrix address not to transfer data from the FPGA     !!
    // !! Just trigger read operation to transfer data from FPGA on-chip to off-chip !!
    for (int k=0; k<=374; k++) ((char *)input_buf)[k] = false;
    ((char *)input_buf)[375] = true; // op_valid
    ((char *)input_buf)[376] = false;
    ((char *)input_buf)[377] = true; // op_bits
    for (int k=378; k<=389; k++) ((char *)input_buf)[k] = false;

    init_buffer(output_buf, 32);
    while (output_buf[0] == false) {;}
    init_buffer(input_buf, 32);

    // Read model weights from the fpga
    for (int i=0; i<n; i++) {
        int current_column = i;
        for (int k=0; k<=360; k++) ((char *)input_buf)[k] = false;
        ((char *)input_buf)[361] = true; // read_req
        ((char *)input_buf)[362] = (current_column >> 5) & 0x1;
        ((char *)input_buf)[363] = (current_column >> 4) & 0x1;
        ((char *)input_buf)[364] = (current_column >> 3) & 0x1;
        ((char *)input_buf)[365] = (current_column >> 2) & 0x1;
        ((char *)input_buf)[366] = (current_column >> 1) & 0x1;
        ((char *)input_buf)[367] = (current_column >> 0) & 0x1; // read_column
        ((char *)input_buf)[368] = false;
        ((char *)input_buf)[369] = false;
        ((char *)input_buf)[370] = false;
        ((char *)input_buf)[371] = false;
        ((char *)input_buf)[372] = false;
        ((char *)input_buf)[373] = false;
        ((char *)input_buf)[374] = false; // read_row
        for (int k=375; k<=389; k++) ((char *)input_buf)[k] = false;
        
        init_buffer(output_buf, 32);
        while (output_buf[0] == false) {;}
        init_buffer(input_buf, 32);

        model_weights[current_column + 0] = output_buf[1 + 0];
        model_weights[current_column + 1] = output_buf[1 + 1];
        model_weights[current_column + 2] = output_buf[1 + 2];
        model_weights[current_column + 3] = output_buf[1 + 3];
        model_weights[current_column + 4] = output_buf[1 + 4];
        model_weights[current_column + 5] = output_buf[1 + 5];
        model_weights[current_column + 6] = output_buf[1 + 6];
        model_weights[current_column + 7] = output_buf[1 + 7];
    }
    return;
}

inline void entire_training(
    float *a, int m,
    float *b, int n,
    float **cached_matrix_ptr,
    float *model_weights
) {
    if (!init_flag) { 
        COUT_N_EXIT("FPGA is not initialized.");
    }

    // Check shared memory space
    assert(input_buf);
    assert(output_buf);

    // Allocate shared memory space for cached matrix
    if (!(*cached_matrix_ptr)) {
        auto cached_matrix_handle = fpga.allocBuffer(getpagesize());
        *cached_matrix_ptr = reinterpret_cast<volatile fpga_float*>(output_buf_handle->c_type());
        if (!(*cached_matrix_ptr)) return -1;
    }
    float *cached_matrix = *cached_matrix_ptr;
    
    // Tell FPGA shared memory addresses
    csrs.writeCSR(1, intptr_t(input_buf));
    csrs.writeCSR(2, intptr_t(output_buf));
    csrs.writeCSR(3, intptr_t(cached_matrix));

    init_buffer(input_buf, 32);
    init_buffer(output_buf, 32);
    init_buffer(cached_matrix, 32);

    // Write a matrix to the fpga
    for (int i=0; i<n; i++) {
        int current_column = i;
        for (int j=0; j<m; j+=8) {
            int current_row_start = j;
            ((char *)input_buf)[0] = true; // write_req_valid
            ((char *)input_buf)[1] = true; // write_req_bits
            ((char *)input_buf)[2] = (current_column >> 5) & 0x1;
            ((char *)input_buf)[3] = (current_column >> 4) & 0x1;
            ((char *)input_buf)[4] = (current_column >> 3) & 0x1;
            ((char *)input_buf)[5] = (current_column >> 2) & 0x1;
            ((char *)input_buf)[6] = (current_column >> 1) & 0x1;
            ((char *)input_buf)[7] = (current_column >> 0) & 0x1; // write_column
            ((char *)input_buf)[8] = (current_row_start >> 6) & 0x1;
            ((char *)input_buf)[9] = (current_row_start >> 5) & 0x1;
            ((char *)input_buf)[10] = (current_row_start >> 4) & 0x1;
            ((char *)input_buf)[11] = (current_row_start >> 3) & 0x1;
            ((char *)input_buf)[12] = (current_row_start >> 2) & 0x1;
            ((char *)input_buf)[13] = (current_row_start >> 1) & 0x1;
            ((char *)input_buf)[14] = (current_row_start >> 0) & 0x1; // write_row
            (fpga_float *)((char *)input_buf)[15] = a[(current_row_start + 0) * n + current_column];
            (fpga_float *)((char *)input_buf)[47] = a[(current_row_start + 1) * n + current_column];
            (fpga_float *)((char *)input_buf)[79] = a[(current_row_start + 2) * n + current_column];
            (fpga_float *)((char *)input_buf)[126] = a[(current_row_start + 3) * n + current_column];
            (fpga_float *)((char *)input_buf)[173] = a[(current_row_start + 4) * n + current_column];
            (fpga_float *)((char *)input_buf)[220] = a[(current_row_start + 5) * n + current_column];
            (fpga_float *)((char *)input_buf)[267] = a[(current_row_start + 6) * n + current_column];
            (fpga_float *)((char *)input_buf)[314] = a[(current_row_start + 7) * n + current_column];
            ((char *)input_buf)[361] = false; // read_req
            ((char *)input_buf)[362] = false;
            ((char *)input_buf)[363] = false;
            ((char *)input_buf)[364] = false;
            ((char *)input_buf)[365] = false;
            ((char *)input_buf)[366] = false; 
            ((char *)input_buf)[367] = false; // read_column
            ((char *)input_buf)[368] = false;
            ((char *)input_buf)[369] = false;
            ((char *)input_buf)[370] = false;
            ((char *)input_buf)[371] = false;
            ((char *)input_buf)[372] = false;
            ((char *)input_buf)[373] = false;
            ((char *)input_buf)[374] = false; // read_row
            ((char *)input_buf)[375] = false; // op_valid
            ((char *)input_buf)[376] = false;
            ((char *)input_buf)[377] = false; // op_bits
            ((char *)input_buf)[378] = false; // used_pus_valid
            ((char *)input_buf)[379] = false; // used_pus_bits
            ((char *)input_buf)[380] = false;
            ((char *)input_buf)[381] = false;
            ((char *)input_buf)[382] = false;
            ((char *)input_buf)[383] = false;
            ((char *)input_buf)[384] = false; // length_bits_0
            ((char *)input_buf)[385] = false;
            ((char *)input_buf)[386] = false;
            ((char *)input_buf)[387] = false;
            ((char *)input_buf)[388] = false;
            ((char *)input_buf)[389] = false; // length_bits_1

            init_buffer(output_buf, 32);
            while (output_buf[0] == false) {;}
            init_buffer(input_buf, 32);
        }
    }

    // Write b matrix to the fpga
    for (int i=0; i<m; i+=8) {
        int current_row_start = i;
        ((char *)input_buf)[0] = true; // write_req_valid
        ((char *)input_buf)[1] = false; // write_req_bits
        ((char *)input_buf)[2] = false;
        ((char *)input_buf)[3] = false;
        ((char *)input_buf)[4] = false;
        ((char *)input_buf)[5] = false;
        ((char *)input_buf)[6] = false;
        ((char *)input_buf)[7] = false; // write_column
        ((char *)input_buf)[8] = (current_row_start >> 6) & 0x1;
        ((char *)input_buf)[9] = (current_row_start >> 5) & 0x1;
        ((char *)input_buf)[10] = (current_row_start >> 4) & 0x1;
        ((char *)input_buf)[11] = (current_row_start >> 3) & 0x1;
        ((char *)input_buf)[12] = (current_row_start >> 2) & 0x1;
        ((char *)input_buf)[13] = (current_row_start >> 1) & 0x1;
        ((char *)input_buf)[14] = (current_row_start >> 0) & 0x1; // write_row
        (fpga_float *)((char *)input_buf)[15] = b[(current_row_start + 0)];
        (fpga_float *)((char *)input_buf)[47] = b[(current_row_start + 1)];
        (fpga_float *)((char *)input_buf)[79] = b[(current_row_start + 2)];
        (fpga_float *)((char *)input_buf)[126] = b[(current_row_start + 3)];
        (fpga_float *)((char *)input_buf)[173] = b[(current_row_start + 4)];
        (fpga_float *)((char *)input_buf)[220] = b[(current_row_start + 5)];
        (fpga_float *)((char *)input_buf)[267] = b[(current_row_start + 6)];
        (fpga_float *)((char *)input_buf)[314] = b[(current_row_start + 7)];
        ((char *)input_buf)[361] = false; // read_req
        ((char *)input_buf)[362] = false;
        ((char *)input_buf)[363] = false;
        ((char *)input_buf)[364] = false;
        ((char *)input_buf)[365] = false;
        ((char *)input_buf)[366] = false; 
        ((char *)input_buf)[367] = false; // read_column
        ((char *)input_buf)[368] = false;
        ((char *)input_buf)[369] = false;
        ((char *)input_buf)[370] = false;
        ((char *)input_buf)[371] = false;
        ((char *)input_buf)[372] = false;
        ((char *)input_buf)[373] = false;
        ((char *)input_buf)[374] = false; // read_row
        ((char *)input_buf)[375] = false; // op_valid
        ((char *)input_buf)[376] = false;
        ((char *)input_buf)[377] = false; // op_bits
        ((char *)input_buf)[378] = false; // used_pus_valid
        ((char *)input_buf)[379] = false; // used_pus_bits
        ((char *)input_buf)[380] = false;
        ((char *)input_buf)[381] = false;
        ((char *)input_buf)[382] = false;
        ((char *)input_buf)[383] = false;
        ((char *)input_buf)[384] = false; // length_bits_0
        ((char *)input_buf)[385] = false;
        ((char *)input_buf)[386] = false;
        ((char *)input_buf)[387] = false;
        ((char *)input_buf)[388] = false;
        ((char *)input_buf)[389] = false; // length_bits_1

        init_buffer(output_buf, 32);
        while (output_buf[0] == false) {;}
        init_buffer(input_buf, 32);
    }

    // Trigger entire training
    for (int k=0; k<=374; k++) ((char *)input_buf)[k] = false;
    ((char *)input_buf)[375] = true; // op_valid
    ((char *)input_buf)[376] = true;
    ((char *)input_buf)[377] = false; // op_bits
    ((char *)input_buf)[378] = true; // used_pus_valid
    ((char *)input_buf)[379] = true; // used_pus_bits
    ((char *)input_buf)[380] = (m >> 4) & 0x1;
    ((char *)input_buf)[381] = (m >> 3) & 0x1;
    ((char *)input_buf)[382] = (m >> 2) & 0x1;
    ((char *)input_buf)[383] = (m >> 1) & 0x1;
    ((char *)input_buf)[384] = (m >> 0) & 0x1; // length_bits_0
    ((char *)input_buf)[385] = (m >> 4) & 0x1;
    ((char *)input_buf)[386] = (m >> 3) & 0x1;
    ((char *)input_buf)[387] = (m >> 2) & 0x1;
    ((char *)input_buf)[388] = (m >> 1) & 0x1;
    ((char *)input_buf)[389] = (m >> 0) & 0x1; // length_bits_1

    // Sync
    init_buffer(output_buf, 32);
    while (output_buf[0] == false) {;}
    init_buffer(input_buf, 32);

    // "Trigger" read cached_matrix from the fpga
    // !! Do not access cached_matrix address not to transfer data from the FPGA     !!
    // !! Just trigger read operation to transfer data from FPGA on-chip to off-chip !!
    for (int k=0; k<=374; k++) ((char *)input_buf)[k] = false;
    ((char *)input_buf)[375] = true; // op_valid
    ((char *)input_buf)[376] = false;
    ((char *)input_buf)[377] = true; // op_bits
    for (int k=378; k<=389; k++) ((char *)input_buf)[k] = false;

    init_buffer(output_buf, 32);
    while (output_buf[0] == false) {;}
    init_buffer(input_buf, 32);

    // Read model weights from the fpga
    for (int i=0; i<n; i++) {
        int current_column = i;
        for (int k=0; k<=360; k++) ((char *)input_buf)[k] = false;
        ((char *)input_buf)[361] = true; // read_req
        ((char *)input_buf)[362] = (current_column >> 5) & 0x1;
        ((char *)input_buf)[363] = (current_column >> 4) & 0x1;
        ((char *)input_buf)[364] = (current_column >> 3) & 0x1;
        ((char *)input_buf)[365] = (current_column >> 2) & 0x1;
        ((char *)input_buf)[366] = (current_column >> 1) & 0x1;
        ((char *)input_buf)[367] = (current_column >> 0) & 0x1; // read_column
        ((char *)input_buf)[368] = false;
        ((char *)input_buf)[369] = false;
        ((char *)input_buf)[370] = false;
        ((char *)input_buf)[371] = false;
        ((char *)input_buf)[372] = false;
        ((char *)input_buf)[373] = false;
        ((char *)input_buf)[374] = false; // read_row
        for (int k=375; k<=389; k++) ((char *)input_buf)[k] = false;
        
        init_buffer(output_buf, 32);
        while (output_buf[0] == false) {;}
        init_buffer(input_buf, 32);

        model_weights[current_column + 0] = output_buf[1 + 0];
        model_weights[current_column + 1] = output_buf[1 + 1];
        model_weights[current_column + 2] = output_buf[1 + 2];
        model_weights[current_column + 3] = output_buf[1 + 3];
        model_weights[current_column + 4] = output_buf[1 + 4];
        model_weights[current_column + 5] = output_buf[1 + 5];
        model_weights[current_column + 6] = output_buf[1 + 6];
        model_weights[current_column + 7] = output_buf[1 + 7];
    }
    return;
}

}