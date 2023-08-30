`include "cci_mpf_if.vh"
`include "csr_mgr.vh"
`include "afu_json_info.vh"

module app_afu(
    input logic clk,
    cci_mpf_if.to_fiu fiu,      // Connection toward the host.
    app_csrs.app csrs,          // CSR connections
    input logic c0NotEmpty,     // MPF tracks outstanding requests. These will be true as long as
    input logic c1NotEmpty      // reads or unacknowledged writes are still in flight.
);

    // Local reset to reduce fan-out
    logic reset = 1'b1;
    always @(posedge clk)
    begin
        reset <= fiu.reset;
    end

    // =========================================================================
    //   Byte Address (CPU uses) <-> Line Address (FPGA uses)
    // =========================================================================

    /**
     *  CCI-P use 512 bit (64 byte) cache line for data transfer.
     *  Thanks to mpf library, FPGA can use same address space with the host system (CPU).
     *
     *  CPU uses byte addresses, but CCI-P header uses cache line addresses.
     *  Therefore, 6 low bits (CL_BYTE_IDX_BITS) of byte addresses are used as offset bits in line addresses.
     *  - byteAddrToClAddr function removes the 6 offset bits from the byte addresses to get the cache line address
     *  - clAddrToByteAddr function adds 6 offset bits (6'b000000) to the cache line address to get the byte address
     */ 

    localparam CL_BYTE_IDX_BITS = 6;
    typedef logic [$bits(t_cci_clAddr) + CL_BYTE_IDX_BITS - 1 : 0] t_byteAddr;

    function automatic t_cci_clAddr byteAddrToClAddr(t_byteAddr addr);
        return addr[CL_BYTE_IDX_BITS +: $bits(t_cci_clAddr)];
    endfunction

    function automatic t_byteAddr clAddrToByteAddr(t_cci_clAddr addr);
        return {addr, CL_BYTE_IDX_BITS'(0)};
    endfunction

    // =========================================================================
    //   CSR Handling
    // =========================================================================

    // Initialize Read CSRs
    always_comb begin
        csrs.afu_id = `AFU_ACCEL_UUID;

        for (int i=0; i<NUM_APP_CSRS; i=i+1) begin
            csrs.cpu_rd_csrs[i].data = 64'(0);
        end
    end

    // CSR write handling variables
    logic is_fn_written;
    assign is_fn_written = csrs.cpu_wr_csrs[0].en;
    logic is_input_u_buf_written;
    assign is_input_u_buf_written = csrs.cpu_wr_csrs[1].en;
    logic is_input_a_buf_written;
    assign is_input_a_buf_written = csrs.cpu_wr_csrs[2].en;
    logic is_output_buf_written;
    assign is_output_buf_written = csrs.cpu_wr_csrs[3].en;
    logic is_gamma_val_written;
    assign is_gamma_val_written = csrs.cpu_wr_csrs[4].en;
    logic is_reset_signal_written;
    assign is_reset_signal_written = csrs.cpu_wr_csrs[5].en;

    // =========================================================================
    //   Main AFU logic
    // =========================================================================
    typedef enum logic [4:0] {
        STATE_WAITING_INPUT_U,
        STATE_WAITING_INPUT_A,
        STATE_WAITING_GAMMA,
        STATE_WAITING_OUTPUT,
        STATE_IDLE,

        STATE_REQUEST_HEAD_U,   // Just send the request
        STATE_REQUEST_BODY_U,   // Send the request & Receive the response
        STATE_REQUEST_TAIL_U,   // Just receive the response
        STATE_REQUEST_HEAD_A,   // Just send the request
        STATE_REQUEST_BODY_A,   // Send the request & Receive the response
        STATE_REQUEST_TAIL_A,   // Just receive the response
        STATE_FEED_MODULE,
        STATE_WAIT,
        STATE_RESPONSE,

        STATE_RESET
    } t_state;
    t_state state;

    logic [2:0] waiting_counter;        // -- Runtime Variable
    integer i;                          // -- Runtime Variable

    logic [7:0] vector_length_16;       // # of elements (x 16)                 -- It is a constant
    logic [7:0] vector_length_8;        // # of elements (x 8)                  -- It is a constant
    
    logic [7:0] vector_u_request_index;   // \ Offset in the input vector (x 16)
    logic [7:0] vector_u_response_index;  //  |
    logic [7:0] vector_a_request_index;   //  |
    logic [7:0] vector_a_response_index;   // /

    logic [7:0] vector_feed_index;      // \ Offset in the input vector (x 8)
    logic [7:0] vector_result_index;    // /
    logic [7:0] output_request_index;   // Offset in the output vector (x 16)

    logic [31:0] input_u_vector_buffer [255:0];       // \ Buffer for the input vector
    logic [31:0] input_a_vector_buffer [255:0];       // /
    logic [31:0] input_gamma_buffer;                  // Buffer for the output gamma value
    logic [31:0] output_buffer [255:0];               // Buffer for the output vector

    t_ccip_clAddr input_u_head_addr;
    t_ccip_clAddr input_a_head_addr;
    t_ccip_clAddr output_head_addr;

    // Input u buffer Read Header
    t_cci_mpf_c0_ReqMemHdr input_u_buffer_read_hdr;
    t_cci_mpf_ReqMemHdrParams input_u_buffer_read_params;
    always_comb begin
        input_u_buffer_read_params = cci_mpf_defaultReqHdrParams(1);
        input_u_buffer_read_params.vc_sel = eVC_VL0;
        input_u_buffer_read_params.cl_len = eCL_LEN_1;
        input_u_buffer_read_hdr = cci_mpf_c0_genReqHdr(eREQ_RDLINE_I,
                                                     input_u_head_addr + vector_u_request_index,
                                                     t_cci_mdata'(0),
                                                     input_u_buffer_read_params);
    end

    // Input a buffer Read Header
    t_cci_mpf_c0_ReqMemHdr input_a_buffer_read_hdr;
    t_cci_mpf_ReqMemHdrParams input_a_buffer_read_params;
    always_comb begin
        input_a_buffer_read_params = cci_mpf_defaultReqHdrParams(1);
        input_a_buffer_read_params.vc_sel = eVC_VL0;
        input_a_buffer_read_params.cl_len = eCL_LEN_1;
        input_a_buffer_read_hdr = cci_mpf_c0_genReqHdr(eREQ_RDLINE_I,
                                                     input_a_head_addr + vector_a_request_index,
                                                     t_cci_mdata'(0),
                                                     input_a_buffer_read_params);
    end

    // Output buffer Write Header
    t_cci_mpf_c1_ReqMemHdr output_buffer_write_hdr;
    t_cci_mpf_ReqMemHdrParams output_buffer_write_params;
    always_comb begin
        output_buffer_write_params = cci_mpf_defaultReqHdrParams(1);
        output_buffer_write_params.vc_sel = eVC_VL0;
        output_buffer_write_params.cl_len = eCL_LEN_1;
        output_buffer_write_hdr = cci_mpf_c1_genReqHdr(eREQ_WRLINE_I,
                                                       output_head_addr + output_request_index,
                                                       t_cci_mdata'(0),
                                                       output_buffer_write_params);
    end

    // Input Signals
    typedef struct packed {
        logic               io_start_valid;
        logic [7:0]         io_start_bits;
        logic               io_input_u_valid;
        logic [7:0][0:0]    io_input_u_bits_sign;
        logic [7:0][7:0]    io_input_u_bits_exp;
        logic [7:0][22:0]   io_input_u_bits_frac;
        logic               io_input_a_valid;
        logic [7:0][0:0]    io_input_a_bits_sign;
        logic [7:0][7:0]    io_input_a_bits_exp;
        logic [7:0][22:0]   io_input_a_bits_frac;
        logic               io_input_gamma_valid;
        logic               io_input_gamma_bits_sign;
        logic [7:0]         io_input_gamma_bits_exp;
        logic [22:0]        io_input_gamma_bits_frac;
    } InputStruct;

    // Output Signals
    typedef struct packed {
        logic               io_output_valid;
        logic [7:0][0:0]    io_output_bits_sign;
        logic [7:0][7:0]    io_output_bits_exp;
        logic [7:0][22:0]   io_output_bits_frac;
        logic               io_done;
    } OutputStruct;

    logic module_reset;
    InputStruct input_str;
    OutputStruct output_str;

    assign module_reset = (reset || (state == STATE_RESET));

    // DUT
    InnerLoopPE inner_loop_pe_unit (
        .clock(clk),
        .reset(module_reset),
        .io_start_valid(input_str.io_start_valid),
        .io_start_bits(input_str.io_start_bits),
        .io_input_u_valid(input_str.io_input_u_valid),
        .io_input_u_bits_0_sign(input_str.io_input_u_bits_sign[0]),
        .io_input_u_bits_0_exp(input_str.io_input_u_bits_exp[0]),
        .io_input_u_bits_0_frac(input_str.io_input_u_bits_frac[0]),
        .io_input_u_bits_1_sign(input_str.io_input_u_bits_sign[1]),
        .io_input_u_bits_1_exp(input_str.io_input_u_bits_exp[1]),
        .io_input_u_bits_1_frac(input_str.io_input_u_bits_frac[1]),
        .io_input_u_bits_2_sign(input_str.io_input_u_bits_sign[2]),
        .io_input_u_bits_2_exp(input_str.io_input_u_bits_exp[2]),
        .io_input_u_bits_2_frac(input_str.io_input_u_bits_frac[2]),
        .io_input_u_bits_3_sign(input_str.io_input_u_bits_sign[3]),
        .io_input_u_bits_3_exp(input_str.io_input_u_bits_exp[3]),
        .io_input_u_bits_3_frac(input_str.io_input_u_bits_frac[3]),
        .io_input_u_bits_4_sign(input_str.io_input_u_bits_sign[4]),
        .io_input_u_bits_4_exp(input_str.io_input_u_bits_exp[4]),
        .io_input_u_bits_4_frac(input_str.io_input_u_bits_frac[4]),
        .io_input_u_bits_5_sign(input_str.io_input_u_bits_sign[5]),
        .io_input_u_bits_5_exp(input_str.io_input_u_bits_exp[5]),
        .io_input_u_bits_5_frac(input_str.io_input_u_bits_frac[5]),
        .io_input_u_bits_6_sign(input_str.io_input_u_bits_sign[6]),
        .io_input_u_bits_6_exp(input_str.io_input_u_bits_exp[6]),
        .io_input_u_bits_6_frac(input_str.io_input_u_bits_frac[6]),
        .io_input_u_bits_7_sign(input_str.io_input_u_bits_sign[7]),
        .io_input_u_bits_7_exp(input_str.io_input_u_bits_exp[7]),
        .io_input_u_bits_7_frac(input_str.io_input_u_bits_frac[7]),
        .io_input_a_valid(input_str.io_input_a_valid),
        .io_input_a_bits_0_sign(input_str.io_input_a_bits_sign[0]),
        .io_input_a_bits_0_exp(input_str.io_input_a_bits_exp[0]),
        .io_input_a_bits_0_frac(input_str.io_input_a_bits_frac[0]),
        .io_input_a_bits_1_sign(input_str.io_input_a_bits_sign[1]),
        .io_input_a_bits_1_exp(input_str.io_input_a_bits_exp[1]),
        .io_input_a_bits_1_frac(input_str.io_input_a_bits_frac[1]),
        .io_input_a_bits_2_sign(input_str.io_input_a_bits_sign[2]),
        .io_input_a_bits_2_exp(input_str.io_input_a_bits_exp[2]),
        .io_input_a_bits_2_frac(input_str.io_input_a_bits_frac[2]),
        .io_input_a_bits_3_sign(input_str.io_input_a_bits_sign[3]),
        .io_input_a_bits_3_exp(input_str.io_input_a_bits_exp[3]),
        .io_input_a_bits_3_frac(input_str.io_input_a_bits_frac[3]),
        .io_input_a_bits_4_sign(input_str.io_input_a_bits_sign[4]),
        .io_input_a_bits_4_exp(input_str.io_input_a_bits_exp[4]),
        .io_input_a_bits_4_frac(input_str.io_input_a_bits_frac[4]),
        .io_input_a_bits_5_sign(input_str.io_input_a_bits_sign[5]),
        .io_input_a_bits_5_exp(input_str.io_input_a_bits_exp[5]),
        .io_input_a_bits_5_frac(input_str.io_input_a_bits_frac[5]),
        .io_input_a_bits_6_sign(input_str.io_input_a_bits_sign[6]),
        .io_input_a_bits_6_exp(input_str.io_input_a_bits_exp[6]),
        .io_input_a_bits_6_frac(input_str.io_input_a_bits_frac[6]),
        .io_input_a_bits_7_sign(input_str.io_input_a_bits_sign[7]),
        .io_input_a_bits_7_exp(input_str.io_input_a_bits_exp[7]),
        .io_input_a_bits_7_frac(input_str.io_input_a_bits_frac[7]),
        .io_input_gamma_valid(input_str.io_input_gamma_valid),
        .io_input_gamma_bits_sign(input_str.io_input_gamma_bits_sign),
        .io_input_gamma_bits_exp(input_str.io_input_gamma_bits_exp),
        .io_input_gamma_bits_frac(input_str.io_input_gamma_bits_frac),

        .io_output_valid(output_str.io_output_valid),
        .io_output_bits_0_sign(output_str.io_output_bits_sign[0]),
        .io_output_bits_0_exp(output_str.io_output_bits_exp[0]),
        .io_output_bits_0_frac(output_str.io_output_bits_frac[0]),
        .io_output_bits_1_sign(output_str.io_output_bits_sign[1]),
        .io_output_bits_1_exp(output_str.io_output_bits_exp[1]),
        .io_output_bits_1_frac(output_str.io_output_bits_frac[1]),
        .io_output_bits_2_sign(output_str.io_output_bits_sign[2]),
        .io_output_bits_2_exp(output_str.io_output_bits_exp[2]),
        .io_output_bits_2_frac(output_str.io_output_bits_frac[2]),
        .io_output_bits_3_sign(output_str.io_output_bits_sign[3]),
        .io_output_bits_3_exp(output_str.io_output_bits_exp[3]),
        .io_output_bits_3_frac(output_str.io_output_bits_frac[3]),
        .io_output_bits_4_sign(output_str.io_output_bits_sign[4]),
        .io_output_bits_4_exp(output_str.io_output_bits_exp[4]),
        .io_output_bits_4_frac(output_str.io_output_bits_frac[4]),
        .io_output_bits_5_sign(output_str.io_output_bits_sign[5]),
        .io_output_bits_5_exp(output_str.io_output_bits_exp[5]),
        .io_output_bits_5_frac(output_str.io_output_bits_frac[5]),
        .io_output_bits_6_sign(output_str.io_output_bits_sign[6]),
        .io_output_bits_6_exp(output_str.io_output_bits_exp[6]),
        .io_output_bits_6_frac(output_str.io_output_bits_frac[6]),
        .io_output_bits_7_sign(output_str.io_output_bits_sign[7]),
        .io_output_bits_7_exp(output_str.io_output_bits_exp[7]),
        .io_output_bits_7_frac(output_str.io_output_bits_frac[7]),
        .io_done(output_str.io_done)
    );

    // State Machine
    always_ff @(posedge clk) begin
        if(reset) begin
            input_str <= '0;

            fiu.c0Tx.valid <= 1'b0;
            fiu.c1Tx.valid <= 1'b0;

            state <= STATE_WAITING_INPUT_U;
        end else begin
            if ((state == STATE_WAITING_INPUT_U) && is_input_u_buf_written) begin
                $display("AFU read input U buffer address");
                state <= STATE_WAITING_INPUT_A;

                input_u_head_addr <= byteAddrToClAddr(csrs.cpu_wr_csrs[1].data);
            end else if((state == STATE_WAITING_INPUT_A) && is_input_a_buf_written) begin
                $display("AFU read input A buffer address");
                state <= STATE_WAITING_GAMMA;

                input_a_head_addr <= byteAddrToClAddr(csrs.cpu_wr_csrs[2].data);
            end else if((state == STATE_WAITING_GAMMA) && is_gamma_val_written) begin
                $display("AFU read gamma value: %b", csrs.cpu_wr_csrs[4].data[31:0]);
                state <= STATE_WAITING_OUTPUT;

                input_gamma_buffer <= csrs.cpu_wr_csrs[4].data[31:0];
            end else if((state == STATE_WAITING_OUTPUT) && is_output_buf_written) begin
                $display("AFU read Output buffer address");
                state <= STATE_IDLE;

                output_head_addr <= byteAddrToClAddr(csrs.cpu_wr_csrs[3].data);
            end else if(state == STATE_IDLE) begin
                fiu.c0Tx.valid <= 1'b0;
                fiu.c1Tx.valid <= 1'b0;

                if(is_fn_written) begin
                    $display("AFU got start signal, send it to the Inner Loop PE");
                    state <= STATE_REQUEST_HEAD_U;

                    vector_length_16 <= csrs.cpu_wr_csrs[0].data[7:1] + csrs.cpu_wr_csrs[0].data[0];
                    vector_length_8 <= csrs.cpu_wr_csrs[0].data[7:0];

                    vector_u_request_index <= 0;
                    vector_a_request_index <= 0;
                    vector_u_response_index <= 0;
                    vector_a_response_index <= 0;
                    vector_feed_index <= 0;
                    vector_result_index <= 0;
                    output_request_index <= 0;

                    waiting_counter <= 0;
                end else if(is_reset_signal_written) begin
                    $display("AFU got reset signal, send it to the Outer Loop PE");
                    state <= STATE_RESET;
                end else begin
                    state <= STATE_IDLE;
                end
            end else if(state == STATE_REQUEST_HEAD_U) begin
                $display("AFU started to send input U buffer read request");
                $display("Remain Vector: (%d)", vector_length_16 - vector_u_request_index);

                if (vector_length_16 - vector_u_request_index == 1) begin
                    state <= STATE_REQUEST_TAIL_U;
                end else begin
                    state <= STATE_REQUEST_BODY_U;
                end

                fiu.c0Tx.valid <= 1'b1;
                fiu.c0Tx.hdr <= input_u_buffer_read_hdr;

                vector_u_request_index <= vector_u_request_index + 1;
            end else if(state == STATE_REQUEST_BODY_U) begin
                $display("AFU sent input U buffer read request");
                $display("Remain Vector: (%d)", vector_length_16 - vector_u_request_index);

                if (vector_length_16 - vector_u_request_index == 1) begin
                    state <= STATE_REQUEST_TAIL_U;
                end

                // Read request logic
                fiu.c0Tx.valid <= 1'b1;
                fiu.c0Tx.hdr <= input_u_buffer_read_hdr;

                vector_u_request_index <= vector_u_request_index + 1;

                // Read response logic
                if(cci_c0Rx_isReadRsp(fiu.c0Rx)) begin
                    $display("AFU got U buffer read response");
                    $display("Remain Vector: (%d)", vector_length_16 - vector_u_response_index);

                    $write("U Input vector elements - ");
                    for (i=0; i<16; i++) begin
                        $write("(%d: %b) ", i, fiu.c0Rx.data[i*32 +: 32]);
                        input_u_vector_buffer[i + vector_u_response_index * 16] <= fiu.c0Rx.data[i*32 +: 32];
                    end
                    $write("\n");

                    vector_u_response_index <= vector_u_response_index + 1;
                end
            end else if(state == STATE_REQUEST_TAIL_U) begin
                fiu.c0Tx.valid <= 1'b0;

                // Print debug msg
                if (vector_length_16 != vector_u_request_index) begin
                    $display("vector_length_16 != vector_u_request_index");
                end

                if (vector_length_16 == vector_u_response_index) begin
                    state <= STATE_REQUEST_HEAD_A;
                end

                if(cci_c0Rx_isReadRsp(fiu.c0Rx)) begin
                    $display("AFU got U buffer read response");
                    $display("Remain Vector: (%d)", vector_length_16 - vector_u_response_index);

                    $write("U Input vector elements - ");
                    for (i=0; i<16; i++) begin
                        $write("(%d: %b) ", i, fiu.c0Rx.data[i*32 +: 32]);
                        input_u_vector_buffer[i + vector_u_response_index * 16] <= fiu.c0Rx.data[i*32 +: 32];
                    end
                    $write("\n");

                    vector_u_response_index <= vector_u_response_index + 1;
                end
            end else if(state == STATE_REQUEST_HEAD_A) begin
                $display("AFU started to send input A buffer read request");
                $display("Remain Vector: (%d)", vector_length_16 - vector_a_request_index);

                if (vector_length_16 - vector_a_request_index == 1) begin
                    state <= STATE_REQUEST_TAIL_A;
                end else begin
                    state <= STATE_REQUEST_BODY_A;
                end

                fiu.c0Tx.valid <= 1'b1;
                fiu.c0Tx.hdr <= input_a_buffer_read_hdr;

                vector_a_request_index <= vector_a_request_index + 1;
            end else if(state == STATE_REQUEST_BODY_A) begin
                $display("AFU sent input A buffer read request");
                $display("Remain Vector: (%d)", vector_length_16 - vector_a_request_index);

                if (vector_length_16 - vector_a_request_index == 1) begin
                    state <= STATE_REQUEST_TAIL_A;
                end

                // Read request logic
                fiu.c0Tx.valid <= 1'b1;
                fiu.c0Tx.hdr <= input_a_buffer_read_hdr;

                vector_a_request_index <= vector_a_request_index + 1;

                // Read response logic
                if(cci_c0Rx_isReadRsp(fiu.c0Rx)) begin
                    $display("AFU got A buffer read response");
                    $display("Remain Vector: (%d)", vector_length_16 - vector_a_response_index);

                    $write("A Input vector elements - ");
                    for (i=0; i<16; i++) begin
                        $write("(%d: %b) ", i, fiu.c0Rx.data[i*32 +: 32]);
                        input_a_vector_buffer[i + vector_a_response_index * 16] <= fiu.c0Rx.data[i*32 +: 32];
                    end
                    $write("\n");

                    vector_a_response_index <= vector_a_response_index + 1;
                end
            end else if(state == STATE_REQUEST_TAIL_A) begin
                fiu.c0Tx.valid <= 1'b0;

                // Print debug msg
                if (vector_length_16 != vector_a_request_index) begin
                    $display("vector_length_16 != vector_a_request_index");
                end

                if (vector_length_16 == vector_a_response_index) begin
                    state <= STATE_FEED_MODULE;
                end

                if(cci_c0Rx_isReadRsp(fiu.c0Rx)) begin
                    $display("AFU got A buffer read response");
                    $display("Remain Vector: (%d)", vector_length_16 - vector_a_response_index);

                    $write("A Input vector elements - ");
                    for (i=0; i<16; i++) begin
                        $write("(%d: %b) ", i, fiu.c0Rx.data[i*32 +: 32]);
                        input_a_vector_buffer[i + vector_a_response_index * 16] <= fiu.c0Rx.data[i*32 +: 32];
                    end
                    $write("\n");

                    vector_a_response_index <= vector_a_response_index + 1;
                end
            end else if(state == STATE_FEED_MODULE) begin
                $display("Inner Loop PE got the part of the input vectors");
                $display("Current counter: (%d)", waiting_counter);

                // FSM logic
                if ((vector_length_8 - vector_feed_index == 1) && (waiting_counter == 0)) begin
                    state <= STATE_WAIT;
                end

                if (waiting_counter == 0) begin
                    waiting_counter <= 3'd4;

                    // Input
                    $display("Feed input! (%d)", vector_feed_index);
                    if (vector_feed_index == 0) begin
                        $display("Start Signal: %d", vector_length_8);
                        input_str.io_start_valid <= 1'b1;
                        input_str.io_start_bits <= vector_length_8;
                        input_str.io_input_gamma_valid <= 1'b1;
                        input_str.io_input_gamma_bits_sign <= input_gamma_buffer[31:31];
                        input_str.io_input_gamma_bits_exp <= input_gamma_buffer[30:23];
                        input_str.io_input_gamma_bits_frac <= input_gamma_buffer[22:0];
                    end else begin
                        input_str.io_start_valid <= 1'b0;
                        input_str.io_start_bits <= 8'b0;
                        input_str.io_input_gamma_valid <= 1'b0;
                        input_str.io_input_gamma_bits_sign <= 1'b0;
                        input_str.io_input_gamma_bits_exp <= 8'b0;
                        input_str.io_input_gamma_bits_frac <= 23'b0;
                    end

                    input_str.io_input_a_valid <= 1'b1;
                    $write("A Input feed elements - ");
                    for (i=0; i<8; i++) begin
                        $write("(%d: %b) ", i, input_a_vector_buffer[i + vector_feed_index * 8]);
                        input_str.io_input_a_bits_sign[i] <= input_a_vector_buffer[i + vector_feed_index * 8][31:31];
                        input_str.io_input_a_bits_exp[i] <= input_a_vector_buffer[i + vector_feed_index * 8][30:23];
                        input_str.io_input_a_bits_frac[i] <= input_a_vector_buffer[i + vector_feed_index * 8][22:0];
                    end
                    $write("\n");

                    input_str.io_input_u_valid <= 1'b1;
                    $write("U Input feed elements - ");
                    for (i=0; i<8; i++) begin
                        $write("(%d: %b) ", i, input_u_vector_buffer[i + vector_feed_index * 8]);
                        input_str.io_input_u_bits_sign[i] <= input_u_vector_buffer[i + vector_feed_index * 8][31:31];
                        input_str.io_input_u_bits_exp[i] <= input_u_vector_buffer[i + vector_feed_index * 8][30:23];
                        input_str.io_input_u_bits_frac[i] <= input_u_vector_buffer[i + vector_feed_index * 8][22:0];
                    end
                    $write("\n");

                    vector_feed_index <= vector_feed_index + 1;
                end else begin
                    waiting_counter <= waiting_counter - 1;
                    input_str <= '0;
                end
            end else if(state == STATE_WAIT) begin
                $display("AFU is waiting for Inner Loop PE to be done");
                input_str <= '0;

                if (output_str.io_done == 1'b1) begin
                    state <= STATE_RESPONSE;
                end

                // Get Output
                if (output_str.io_output_valid == 1'b1) begin
                    $display("Got part of output (%d)", vector_result_index);

                    for (i=0; i<8; i++) begin
                        $write("(%d: %b) ", i, {output_str.io_output_bits_sign[i], output_str.io_output_bits_exp[i], output_str.io_output_bits_frac[i]});
                        output_buffer[i + vector_result_index * 8][31:31] <= output_str.io_output_bits_sign[i];
                        output_buffer[i + vector_result_index * 8][30:23] <= output_str.io_output_bits_exp[i];
                        output_buffer[i + vector_result_index * 8][22:0] <= output_str.io_output_bits_frac[i];
                    end
                    $write("\n");

                    vector_result_index <= vector_result_index + 1;
                end
            end else if(state == STATE_RESPONSE) begin
                $display("AFU started to send output buffer write request");

                if (vector_length_16 - output_request_index == 1) begin
                    state <= STATE_IDLE;
                end

                fiu.c1Tx.valid <= 1'b1;
                fiu.c1Tx.hdr <= output_buffer_write_hdr;

                $write("Output vector elements - ");
                for (i=0; i<16; i++) begin
                    $write("(%d: %b) ", i, output_buffer[i + output_request_index * 16]);
                    fiu.c1Tx.data[i*32 +: 32] <= output_buffer[i + output_request_index * 16];
                end
                $write("\n");

                output_request_index <= output_request_index + 1;
            end else if(state == STATE_RESET) begin 
                $display("Reset Outer Loop PE");
                state <= STATE_IDLE;

                input_str = '0;
            end else begin
                fiu.c0Tx.valid <= 1'b0;
                fiu.c1Tx.valid <= 1'b0;

                input_str = '0;
            end
        end
    end

    assign fiu.c2Tx.mmioRdValid = 1'b0;

endmodule