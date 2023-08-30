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
    logic is_input_buf_written;
    assign is_input_buf_written = csrs.cpu_wr_csrs[1].en;
    logic is_output_buf_written;
    assign is_output_buf_written = csrs.cpu_wr_csrs[2].en;
    logic is_reset_signal_written;
    assign is_reset_signal_written = csrs.cpu_wr_csrs[3].en;
    logic is_gamma_buf_written;
    assign is_gamma_buf_written = csrs.cpu_wr_csrs[4].en;

    // =========================================================================
    //   Main AFU logic
    // =========================================================================
    typedef enum logic [4:0] {
        STATE_WAITING_INPUT,
        STATE_WAITING_OUTPUT,
        STATE_WAITING_GAMMA,
        STATE_IDLE,

        STATE_REQUEST_HEAD,   // Just send the request
        STATE_REQUEST_BODY,   // Send the request & Receive the response
        STATE_REQUEST_TAIL,   // Just receive the response
        STATE_FEED_MODULE,
        STATE_WAIT,
        STATE_RESPONSE_REF,
        STATE_RESPONSE_GAMMA,

        STATE_RESET
    } t_state;
    t_state state;

    logic [2:0] waiting_counter;
    
    integer i;

    logic [7:0] vector_length_16;       // # of elements (x 16)                 -- It is a constant
    logic [7:0] vector_length_8;        // # of elements (x 8)                  -- It is a constant
    logic [7:0] vector_request_index;   // \ Offset in the input vector (x 16)
    logic [7:0] vector_response_index;  // /
    logic [7:0] vector_feed_index;      // \ Offset in the input vector (x 8)
    logic [7:0] vector_result_index;    // /

    logic [7:0] output_request_index;   // Offset in the output vector (x 16)

    logic [31:0] input_vector_buffer [255:0];       // Buffer for the input vector
    logic [31:0] output_reflector_buffer [255:0];   // Buffer for the output reflector vector
    logic [31:0] output_gamma_buffer;               // Buffer for the output gamma value

    t_ccip_clAddr input_head_addr;
    t_ccip_clAddr output_head_addr;
    t_ccip_clAddr gamma_addr;

    // Input buffer Read Header
    t_cci_mpf_c0_ReqMemHdr input_buffer_read_hdr;
    t_cci_mpf_ReqMemHdrParams input_buffer_read_params;
    always_comb begin
        input_buffer_read_params = cci_mpf_defaultReqHdrParams(1);
        input_buffer_read_params.vc_sel = eVC_VL0;
        input_buffer_read_params.cl_len = eCL_LEN_1;
        input_buffer_read_hdr = cci_mpf_c0_genReqHdr(eREQ_RDLINE_I,
                                                     input_head_addr + vector_request_index,
                                                     t_cci_mdata'(0),
                                                     input_buffer_read_params);
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

    // Gamma buffer Write Header
    t_cci_mpf_c1_ReqMemHdr gamma_buffer_write_hdr;
    t_cci_mpf_ReqMemHdrParams gamma_buffer_write_params;
    always_comb begin
        gamma_buffer_write_params = cci_mpf_defaultReqHdrParams(1);
        gamma_buffer_write_params.vc_sel = eVC_VL0;
        gamma_buffer_write_params.cl_len = eCL_LEN_1;
        gamma_buffer_write_hdr = cci_mpf_c1_genReqHdr(eREQ_WRLINE_I,
                                                       gamma_addr,
                                                       t_cci_mdata'(0),
                                                       gamma_buffer_write_params);
    end

    // Input Signals
    typedef struct packed {
        logic               io_start_valid;
        logic [7:0]         io_start_bits;
        logic               io_input_valid;
        logic [7:0][0:0]    io_input_bits_sign;
        logic [7:0][7:0]    io_input_bits_exp;
        logic [7:0][22:0]   io_input_bits_frac; 
    } InputStruct;

    typedef struct packed {
        logic               io_output_reflector_valid;
        logic [7:0][0:0]    io_output_reflector_bits_sign;
        logic [7:0][7:0]    io_output_reflector_bits_exp;
        logic [7:0][22:0]   io_output_reflector_bits_frac;
        logic               io_output_gamma_valid;
        logic               io_output_gamma_bits_sign;
        logic [7:0]         io_output_gamma_bits_exp;
        logic [22:0]        io_output_gamma_bits_frac;
    } OutputStruct;

    logic module_reset;
    InputStruct input_str;
    OutputStruct output_str;

    assign module_reset = (reset || (state == STATE_RESET));

    // DUT
    OuterLoopPE outer_loop_pe_unit (
        .clock(clk),
        .reset(module_reset),
        .io_start_valid(input_str.io_start_valid),
        .io_start_bits(input_str.io_start_bits),
        .io_input_valid(input_str.io_input_valid),
        .io_input_bits_0_sign(input_str.io_input_bits_sign[0]),
        .io_input_bits_0_exp(input_str.io_input_bits_exp[0]),
        .io_input_bits_0_frac(input_str.io_input_bits_frac[0]),
        .io_input_bits_1_sign(input_str.io_input_bits_sign[1]),
        .io_input_bits_1_exp(input_str.io_input_bits_exp[1]),
        .io_input_bits_1_frac(input_str.io_input_bits_frac[1]),
        .io_input_bits_2_sign(input_str.io_input_bits_sign[2]),
        .io_input_bits_2_exp(input_str.io_input_bits_exp[2]),
        .io_input_bits_2_frac(input_str.io_input_bits_frac[2]),
        .io_input_bits_3_sign(input_str.io_input_bits_sign[3]),
        .io_input_bits_3_exp(input_str.io_input_bits_exp[3]),
        .io_input_bits_3_frac(input_str.io_input_bits_frac[3]),
        .io_input_bits_4_sign(input_str.io_input_bits_sign[4]),
        .io_input_bits_4_exp(input_str.io_input_bits_exp[4]),
        .io_input_bits_4_frac(input_str.io_input_bits_frac[4]),
        .io_input_bits_5_sign(input_str.io_input_bits_sign[5]),
        .io_input_bits_5_exp(input_str.io_input_bits_exp[5]),
        .io_input_bits_5_frac(input_str.io_input_bits_frac[5]),
        .io_input_bits_6_sign(input_str.io_input_bits_sign[6]),
        .io_input_bits_6_exp(input_str.io_input_bits_exp[6]),
        .io_input_bits_6_frac(input_str.io_input_bits_frac[6]),
        .io_input_bits_7_sign(input_str.io_input_bits_sign[7]),
        .io_input_bits_7_exp(input_str.io_input_bits_exp[7]),
        .io_input_bits_7_frac(input_str.io_input_bits_frac[7]),
        
        .io_output_reflector_valid(output_str.io_output_reflector_valid),
        .io_output_reflector_bits_0_sign(output_str.io_output_reflector_bits_sign[0]),
        .io_output_reflector_bits_0_exp(output_str.io_output_reflector_bits_exp[0]),
        .io_output_reflector_bits_0_frac(output_str.io_output_reflector_bits_frac[0]),
        .io_output_reflector_bits_1_sign(output_str.io_output_reflector_bits_sign[1]),
        .io_output_reflector_bits_1_exp(output_str.io_output_reflector_bits_exp[1]),
        .io_output_reflector_bits_1_frac(output_str.io_output_reflector_bits_frac[1]),
        .io_output_reflector_bits_2_sign(output_str.io_output_reflector_bits_sign[2]),
        .io_output_reflector_bits_2_exp(output_str.io_output_reflector_bits_exp[2]),
        .io_output_reflector_bits_2_frac(output_str.io_output_reflector_bits_frac[2]),
        .io_output_reflector_bits_3_sign(output_str.io_output_reflector_bits_sign[3]),
        .io_output_reflector_bits_3_exp(output_str.io_output_reflector_bits_exp[3]),
        .io_output_reflector_bits_3_frac(output_str.io_output_reflector_bits_frac[3]),
        .io_output_reflector_bits_4_sign(output_str.io_output_reflector_bits_sign[4]),
        .io_output_reflector_bits_4_exp(output_str.io_output_reflector_bits_exp[4]),
        .io_output_reflector_bits_4_frac(output_str.io_output_reflector_bits_frac[4]),
        .io_output_reflector_bits_5_sign(output_str.io_output_reflector_bits_sign[5]),
        .io_output_reflector_bits_5_exp(output_str.io_output_reflector_bits_exp[5]),
        .io_output_reflector_bits_5_frac(output_str.io_output_reflector_bits_frac[5]),
        .io_output_reflector_bits_6_sign(output_str.io_output_reflector_bits_sign[6]),
        .io_output_reflector_bits_6_exp(output_str.io_output_reflector_bits_exp[6]),
        .io_output_reflector_bits_6_frac(output_str.io_output_reflector_bits_frac[6]),
        .io_output_reflector_bits_7_sign(output_str.io_output_reflector_bits_sign[7]),
        .io_output_reflector_bits_7_exp(output_str.io_output_reflector_bits_exp[7]),
        .io_output_reflector_bits_7_frac(output_str.io_output_reflector_bits_frac[7]),
        .io_output_gamma_valid(output_str.io_output_gamma_valid),
        .io_output_gamma_bits_sign(output_str.io_output_gamma_bits_sign),
        .io_output_gamma_bits_exp(output_str.io_output_gamma_bits_exp),
        .io_output_gamma_bits_frac(output_str.io_output_gamma_bits_frac)
    );

    // State Machine
    always_ff @(posedge clk) begin
        if(reset) begin
            //input_addr <= t_cci_clAddr'(0);
            //output_addr <= t_cci_clAddr'(0);

            input_str <= '0;

            fiu.c0Tx.valid <= 1'b0;
            fiu.c1Tx.valid <= 1'b0;

            state <= STATE_WAITING_INPUT;
        end else begin
            if((state == STATE_WAITING_INPUT) && is_input_buf_written) begin
                $display("AFU read Input buffer address");
                state <= STATE_WAITING_OUTPUT;

                input_head_addr <= byteAddrToClAddr(csrs.cpu_wr_csrs[1].data);
            end else if((state == STATE_WAITING_OUTPUT) && is_output_buf_written) begin
                $display("AFU read Output buffer address");
                state <= STATE_WAITING_GAMMA;

                output_head_addr <= byteAddrToClAddr(csrs.cpu_wr_csrs[2].data);
            end else if((state == STATE_WAITING_GAMMA) && is_gamma_buf_written) begin
                $display("AFU read Gamma buffer address");
                state <= STATE_IDLE;

                gamma_addr <= byteAddrToClAddr(csrs.cpu_wr_csrs[4].data);
            end else if(state == STATE_IDLE) begin
                fiu.c0Tx.valid <= 1'b0;
                fiu.c1Tx.valid <= 1'b0;

                if(is_fn_written) begin
                    $display("AFU got start signal, send it to the Outer Loop PE");
                    state <= STATE_REQUEST_HEAD;

                    vector_length_16 <= csrs.cpu_wr_csrs[0].data[7:1] + csrs.cpu_wr_csrs[0].data[0];
                    vector_length_8 <= csrs.cpu_wr_csrs[0].data[7:0];

                    vector_request_index <= 0;
                    vector_response_index <= 0;
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
            end else if(state == STATE_REQUEST_HEAD) begin
                $display("AFU started to send input buffer read request");
                $display("Remain Vector: (%d)", vector_length_16 - vector_request_index);

                // FSM logic
                if (vector_length_16 - vector_request_index == 1) begin
                    state <= STATE_REQUEST_TAIL;
                end else begin
                    state <= STATE_REQUEST_BODY;
                end

                // Read request logic
                fiu.c0Tx.valid <= 1'b1;
                fiu.c0Tx.hdr <= input_buffer_read_hdr;

                vector_request_index <= vector_request_index + 1;
            end else if(state == STATE_REQUEST_BODY) begin
                $display("AFU sent input buffer read request");
                $display("Remain Vector: (%d)", vector_length_16 - vector_request_index);

                // FSM logic
                if (vector_length_16 - vector_request_index == 1) begin
                    state <= STATE_REQUEST_TAIL;
                end

                // Read request logic
                fiu.c0Tx.valid <= 1'b1;
                fiu.c0Tx.hdr <= input_buffer_read_hdr;

                vector_request_index <= vector_request_index + 1;

                // Read response logic
                if(cci_c0Rx_isReadRsp(fiu.c0Rx)) begin
                    $display("AFU got read response");
                    $display("Remain Vector: (%d)", vector_length_16 - vector_response_index);

                    $write("Input vector elements - ");
                    for (i=0; i<16; i++) begin
                        $write("(%d: %b) ", i, fiu.c0Rx.data[i*32 +: 32]);
                        input_vector_buffer[i + vector_response_index * 16] <= fiu.c0Rx.data[i*32 +: 32];
                    end
                    $write("\n");

                    vector_response_index <= vector_response_index + 1;
                end
            end else if(state == STATE_REQUEST_TAIL) begin
                fiu.c0Tx.valid <= 1'b0;

                // Print debug msg
                if (vector_length_16 != vector_request_index) begin
                    $display("vector_length_16 != vector_request_index");
                end

                // FSM logic
                if (vector_length_16 == vector_response_index) begin
                    state <= STATE_FEED_MODULE;
                end

                // Read response logic
                if(cci_c0Rx_isReadRsp(fiu.c0Rx)) begin
                    $display("AFU got read response");
                    $display("Remain Vector: (%d)", vector_length_16 - vector_response_index);

                    $write("Input vector elements - ");
                    for (i=0; i<16; i++) begin
                        $write("(%d: %b) ", i, fiu.c0Rx.data[i*32 +: 32]);
                        input_vector_buffer[i + vector_response_index * 16] <= fiu.c0Rx.data[i*32 +: 32];
                    end
                    $write("\n");

                    vector_response_index <= vector_response_index + 1;
                end
            end else if(state == STATE_FEED_MODULE) begin
                $display("Outer Loop PE got the part of the input vector");
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
                    end else begin
                        input_str.io_start_valid <= 1'b0;
                        input_str.io_start_bits <= 8'b0;
                    end

                    input_str.io_input_valid <= 1'b1;
                    for (i=0; i<8; i++) begin
                        $write("(%d: %b) ", i, input_vector_buffer[i + vector_feed_index * 8]);
                        input_str.io_input_bits_sign[i] <= input_vector_buffer[i + vector_feed_index * 8][31:31];
                        input_str.io_input_bits_exp[i] <= input_vector_buffer[i + vector_feed_index * 8][30:23];
                        input_str.io_input_bits_frac[i] <= input_vector_buffer[i + vector_feed_index * 8][22:0];
                    end
                    $write("\n");

                    vector_feed_index <= vector_feed_index + 1;
                end else begin
                    waiting_counter <= waiting_counter - 1;

                    // No input
                    input_str <= '0;
                end

            end else if(state == STATE_WAIT) begin
                $display("AFU is waiting for Outer Loop PE to be done");

                input_str <= '0;
                
                // Get Reflector Result
                if (output_str.io_output_reflector_valid == 1'b1) begin
                    $display("Got reflector result (%d)", vector_result_index);

                    for (i=0; i<8; i++) begin
                        $write("(%d: %b) ", i, {output_str.io_output_reflector_bits_sign[i], output_str.io_output_reflector_bits_exp[i], output_str.io_output_reflector_bits_frac[i]});
                        output_reflector_buffer[i + vector_result_index * 8][31:31] <= output_str.io_output_reflector_bits_sign[i];
                        output_reflector_buffer[i + vector_result_index * 8][30:23] <= output_str.io_output_reflector_bits_exp[i];
                        output_reflector_buffer[i + vector_result_index * 8][22:0] <= output_str.io_output_reflector_bits_frac[i];
                    end
                    $write("\n");

                    vector_result_index <= vector_result_index + 1;
                end

                // Get Gamma Result
                if (output_str.io_output_gamma_valid == 1'b1) begin
                    $display("Got gamma result (%b)", {output_str.io_output_gamma_bits_sign, output_str.io_output_gamma_bits_exp, output_str.io_output_gamma_bits_frac});
                    output_gamma_buffer[31:31] <= output_str.io_output_gamma_bits_sign;
                    output_gamma_buffer[30:23] <= output_str.io_output_gamma_bits_exp;
                    output_gamma_buffer[22:0] <= output_str.io_output_gamma_bits_frac;

                    if (vector_result_index != vector_length_8) begin
                        $display("vector_result_index != vector_length_8");
                    end

                    state <= STATE_RESPONSE_REF;
                end

            end else if(state == STATE_RESPONSE_REF) begin
                $display("AFU started to send output buffer write request for reflector vector");

                if (vector_length_16 - output_request_index == 1) begin
                    state <= STATE_RESPONSE_GAMMA;
                end

                fiu.c1Tx.valid <= 1'b1;
                fiu.c1Tx.hdr <= output_buffer_write_hdr;

                $write("Reflector vector elements - ");
                for (i=0; i<16; i++) begin
                    $write("(%d: %b) ", i, output_reflector_buffer[i + output_request_index * 16]);
                    fiu.c1Tx.data[i*32 +: 32] <= output_reflector_buffer[i + output_request_index * 16];
                end
                $write("\n");
                
                output_request_index <= output_request_index + 1;
            end else if(state == STATE_RESPONSE_GAMMA) begin
                $display("AFU sent output buffer write request for gamma value: %b", output_gamma_buffer);
                state <= STATE_IDLE;

                fiu.c1Tx.valid <= 1'b1;
                fiu.c1Tx.hdr <= gamma_buffer_write_hdr;
                fiu.c1Tx.data <= t_ccip_clData'({448'b0, output_gamma_buffer, 32'b1});
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