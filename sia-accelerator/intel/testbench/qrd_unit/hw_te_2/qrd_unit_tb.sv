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
    logic is_start_written;
    assign is_start_written = csrs.cpu_wr_csrs[1].en;
    logic is_reset_signal_written;
    assign is_reset_signal_written = csrs.cpu_wr_csrs[5].en;

    // =========================================================================
    //   Main AFU logic
    // =========================================================================
    typedef enum logic [4:0] {
        STATE_WAITING_INPUT,
        STATE_IDLE,
        STATE_RUN,
        STATE_OUTPUT,
        STATE_RESET
    } t_state;
    t_state state;

    t_ccip_clAddr input_head_addr;
    t_ccip_clAddr output_head_addr;

    // Input buffer Read Header
    t_cci_mpf_c0_ReqMemHdr input_buffer_read_hdr;
    t_cci_mpf_ReqMemHdrParams input_buffer_read_params;
    always_comb begin
        input_buffer_read_params = cci_mpf_defaultReqHdrParams(1);
        input_buffer_read_params.vc_sel = eVC_VL0;
        input_buffer_read_params.cl_len = eCL_LEN_1;
        input_buffer_read_hdr = cci_mpf_c0_genReqHdr(eREQ_RDLINE_I,
                                                     input_head_addr,
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
                                                       output_head_addr,
                                                       t_cci_mdata'(0),
                                                       output_buffer_write_params);
    end

    // Input Signals
    typedef struct packed {
        logic               io_write_req_valid;
        logic               io_write_req_bits;
        logic [4:0]         io_write_column;
        logic [5:0]         io_write_row;
        logic [7:0][0:0]    io_write_input_sign;
        logic [7:0][7:0]    io_write_input_exp;
        logic [7:0][22:0]   io_write_input_frac;
        logic               io_read_req;
        logic [4:0]         io_read_column;
        logic [5:0]         io_read_row;
        logic               io_op_valid;
        logic [1:0]         io_op_bits;
        logic               io_used_pus_valid;
        logic               io_used_pus_bits;
        logic               io_lengths_valid;
        logic [5:0]         io_lengths_bits_0;
        logic [5:0]         io_lengths_bits_1;
    } InputStruct;

    // Output Signals
    typedef struct packed {
        logic               io_read_output_valid;
        logic [7:0][0:0]    io_read_output_bits_sign;
        logic [7:0][7:0]    io_read_output_bits_exp;
        logic [7:0][22:0]   io_read_output_bits_frac;
        logic               io_done;
    } OutputStruct;

    logic module_reset;
    InputStruct input_str_0;
    InputStruct input_str_1;
    //InputStruct input_str_2;

    OutputStruct output_str_0;
    OutputStruct output_str_1;
    //OutputStruct output_str_2;

    assign module_reset = (reset || (state == STATE_RESET));

    // DUT
    QRD_Unit qrd_unit_unit_0 (
        .clock(clk),
        .reset(module_reset),
        .io_write_req_valid(input_str_0.io_write_req_valid),
        .io_write_req_bits(input_str_0.io_write_req_bits),
        .io_write_column(input_str_0.io_write_column),
        .io_write_row(input_str_0.io_write_row),
        .io_write_input_0_sign(input_str_0.io_write_input_sign[0]),
        .io_write_input_0_exp(input_str_0.io_write_input_exp[0]),
        .io_write_input_0_frac(input_str_0.io_write_input_frac[0]),
        .io_write_input_1_sign(input_str_0.io_write_input_sign[1]),
        .io_write_input_1_exp(input_str_0.io_write_input_exp[1]),
        .io_write_input_1_frac(input_str_0.io_write_input_frac[1]),
        .io_write_input_2_sign(input_str_0.io_write_input_sign[2]),
        .io_write_input_2_exp(input_str_0.io_write_input_exp[2]),
        .io_write_input_2_frac(input_str_0.io_write_input_frac[2]),
        .io_write_input_3_sign(input_str_0.io_write_input_sign[3]),
        .io_write_input_3_exp(input_str_0.io_write_input_exp[3]),
        .io_write_input_3_frac(input_str_0.io_write_input_frac[3]),
        .io_write_input_4_sign(input_str_0.io_write_input_sign[4]),
        .io_write_input_4_exp(input_str_0.io_write_input_exp[4]),
        .io_write_input_4_frac(input_str_0.io_write_input_frac[4]),
        .io_write_input_5_sign(input_str_0.io_write_input_sign[5]),
        .io_write_input_5_exp(input_str_0.io_write_input_exp[5]),
        .io_write_input_5_frac(input_str_0.io_write_input_frac[5]),
        .io_write_input_6_sign(input_str_0.io_write_input_sign[6]),
        .io_write_input_6_exp(input_str_0.io_write_input_exp[6]),
        .io_write_input_6_frac(input_str_0.io_write_input_frac[6]),
        .io_write_input_7_sign(input_str_0.io_write_input_sign[7]),
        .io_write_input_7_exp(input_str_0.io_write_input_exp[7]),
        .io_write_input_7_frac(input_str_0.io_write_input_frac[7]),
        .io_read_req(input_str_0.io_read_req),
        .io_read_column(input_str_0.io_read_column),
        .io_read_row(input_str_0.io_read_row),
        .io_read_output_valid(output_str_0.io_read_output_valid),
        .io_read_output_bits_0_sign(output_str_0.io_read_output_bits_sign[0]),
        .io_read_output_bits_0_exp(output_str_0.io_read_output_bits_exp[0]),
        .io_read_output_bits_0_frac(output_str_0.io_read_output_bits_frac[0]),
        .io_read_output_bits_1_sign(output_str_0.io_read_output_bits_sign[1]),
        .io_read_output_bits_1_exp(output_str_0.io_read_output_bits_exp[1]),
        .io_read_output_bits_1_frac(output_str_0.io_read_output_bits_frac[1]),
        .io_read_output_bits_2_sign(output_str_0.io_read_output_bits_sign[2]),
        .io_read_output_bits_2_exp(output_str_0.io_read_output_bits_exp[2]),
        .io_read_output_bits_2_frac(output_str_0.io_read_output_bits_frac[2]),
        .io_read_output_bits_3_sign(output_str_0.io_read_output_bits_sign[3]),
        .io_read_output_bits_3_exp(output_str_0.io_read_output_bits_exp[3]),
        .io_read_output_bits_3_frac(output_str_0.io_read_output_bits_frac[3]),
        .io_read_output_bits_4_sign(output_str_0.io_read_output_bits_sign[4]),
        .io_read_output_bits_4_exp(output_str_0.io_read_output_bits_exp[4]),
        .io_read_output_bits_4_frac(output_str_0.io_read_output_bits_frac[4]),
        .io_read_output_bits_5_sign(output_str_0.io_read_output_bits_sign[5]),
        .io_read_output_bits_5_exp(output_str_0.io_read_output_bits_exp[5]),
        .io_read_output_bits_5_frac(output_str_0.io_read_output_bits_frac[5]),
        .io_read_output_bits_6_sign(output_str_0.io_read_output_bits_sign[6]),
        .io_read_output_bits_6_exp(output_str_0.io_read_output_bits_exp[6]),
        .io_read_output_bits_6_frac(output_str_0.io_read_output_bits_frac[6]),
        .io_read_output_bits_7_sign(output_str_0.io_read_output_bits_sign[7]),
        .io_read_output_bits_7_exp(output_str_0.io_read_output_bits_exp[7]),
        .io_read_output_bits_7_frac(output_str_0.io_read_output_bits_frac[7]),
        .io_op_valid(input_str_0.io_op_valid),
        .io_op_bits(input_str_0.io_op_bits),
        .io_used_pus_valid(input_str_0.io_used_pus_valid),
        .io_used_pus_bits(input_str_0.io_used_pus_bits),
        .io_lengths_valid(input_str_0.io_lengths_valid),
        .io_lengths_bits_0(input_str_0.io_lengths_bits_0),
        .io_lengths_bits_1(input_str_0.io_lengths_bits_1),
        .io_done(output_str_0.io_done)
    );

    QRD_Unit qrd_unit_unit_1 (
        .clock(clk),
        .reset(module_reset),
        .io_write_req_valid(input_str_1.io_write_req_valid),
        .io_write_req_bits(input_str_1.io_write_req_bits),
        .io_write_column(input_str_1.io_write_column),
        .io_write_row(input_str_1.io_write_row),
        .io_write_input_0_sign(input_str_1.io_write_input_sign[0]),
        .io_write_input_0_exp(input_str_1.io_write_input_exp[0]),
        .io_write_input_0_frac(input_str_1.io_write_input_frac[0]),
        .io_write_input_1_sign(input_str_1.io_write_input_sign[1]),
        .io_write_input_1_exp(input_str_1.io_write_input_exp[1]),
        .io_write_input_1_frac(input_str_1.io_write_input_frac[1]),
        .io_write_input_2_sign(input_str_1.io_write_input_sign[2]),
        .io_write_input_2_exp(input_str_1.io_write_input_exp[2]),
        .io_write_input_2_frac(input_str_1.io_write_input_frac[2]),
        .io_write_input_3_sign(input_str_1.io_write_input_sign[3]),
        .io_write_input_3_exp(input_str_1.io_write_input_exp[3]),
        .io_write_input_3_frac(input_str_1.io_write_input_frac[3]),
        .io_write_input_4_sign(input_str_1.io_write_input_sign[4]),
        .io_write_input_4_exp(input_str_1.io_write_input_exp[4]),
        .io_write_input_4_frac(input_str_1.io_write_input_frac[4]),
        .io_write_input_5_sign(input_str_1.io_write_input_sign[5]),
        .io_write_input_5_exp(input_str_1.io_write_input_exp[5]),
        .io_write_input_5_frac(input_str_1.io_write_input_frac[5]),
        .io_write_input_6_sign(input_str_1.io_write_input_sign[6]),
        .io_write_input_6_exp(input_str_1.io_write_input_exp[6]),
        .io_write_input_6_frac(input_str_1.io_write_input_frac[6]),
        .io_write_input_7_sign(input_str_1.io_write_input_sign[7]),
        .io_write_input_7_exp(input_str_1.io_write_input_exp[7]),
        .io_write_input_7_frac(input_str_1.io_write_input_frac[7]),
        .io_read_req(input_str_1.io_read_req),
        .io_read_column(input_str_1.io_read_column),
        .io_read_row(input_str_1.io_read_row),
        .io_read_output_valid(output_str_1.io_read_output_valid),
        .io_read_output_bits_0_sign(output_str_1.io_read_output_bits_sign[0]),
        .io_read_output_bits_0_exp(output_str_1.io_read_output_bits_exp[0]),
        .io_read_output_bits_0_frac(output_str_1.io_read_output_bits_frac[0]),
        .io_read_output_bits_1_sign(output_str_1.io_read_output_bits_sign[1]),
        .io_read_output_bits_1_exp(output_str_1.io_read_output_bits_exp[1]),
        .io_read_output_bits_1_frac(output_str_1.io_read_output_bits_frac[1]),
        .io_read_output_bits_2_sign(output_str_1.io_read_output_bits_sign[2]),
        .io_read_output_bits_2_exp(output_str_1.io_read_output_bits_exp[2]),
        .io_read_output_bits_2_frac(output_str_1.io_read_output_bits_frac[2]),
        .io_read_output_bits_3_sign(output_str_1.io_read_output_bits_sign[3]),
        .io_read_output_bits_3_exp(output_str_1.io_read_output_bits_exp[3]),
        .io_read_output_bits_3_frac(output_str_1.io_read_output_bits_frac[3]),
        .io_read_output_bits_4_sign(output_str_1.io_read_output_bits_sign[4]),
        .io_read_output_bits_4_exp(output_str_1.io_read_output_bits_exp[4]),
        .io_read_output_bits_4_frac(output_str_1.io_read_output_bits_frac[4]),
        .io_read_output_bits_5_sign(output_str_1.io_read_output_bits_sign[5]),
        .io_read_output_bits_5_exp(output_str_1.io_read_output_bits_exp[5]),
        .io_read_output_bits_5_frac(output_str_1.io_read_output_bits_frac[5]),
        .io_read_output_bits_6_sign(output_str_1.io_read_output_bits_sign[6]),
        .io_read_output_bits_6_exp(output_str_1.io_read_output_bits_exp[6]),
        .io_read_output_bits_6_frac(output_str_1.io_read_output_bits_frac[6]),
        .io_read_output_bits_7_sign(output_str_1.io_read_output_bits_sign[7]),
        .io_read_output_bits_7_exp(output_str_1.io_read_output_bits_exp[7]),
        .io_read_output_bits_7_frac(output_str_1.io_read_output_bits_frac[7]),
        .io_op_valid(input_str_1.io_op_valid),
        .io_op_bits(input_str_1.io_op_bits),
        .io_used_pus_valid(input_str_1.io_used_pus_valid),
        .io_used_pus_bits(input_str_1.io_used_pus_bits),
        .io_lengths_valid(input_str_1.io_lengths_valid),
        .io_lengths_bits_0(input_str_1.io_lengths_bits_0),
        .io_lengths_bits_1(input_str_1.io_lengths_bits_1),
        .io_done(output_str_1.io_done)
    );

    /*QRD_Unit qrd_unit_unit_2 (
        .clock(clk),
        .reset(module_reset),
        .io_write_req_valid(input_str_2.io_write_req_valid),
        .io_write_req_bits(input_str_2.io_write_req_bits),
        .io_write_column(input_str_2.io_write_column),
        .io_write_row(input_str_2.io_write_row),
        .io_write_input_0_sign(input_str_2.io_write_input_sign[0]),
        .io_write_input_0_exp(input_str_2.io_write_input_exp[0]),
        .io_write_input_0_frac(input_str_2.io_write_input_frac[0]),
        .io_write_input_1_sign(input_str_2.io_write_input_sign[1]),
        .io_write_input_1_exp(input_str_2.io_write_input_exp[1]),
        .io_write_input_1_frac(input_str_2.io_write_input_frac[1]),
        .io_write_input_2_sign(input_str_2.io_write_input_sign[2]),
        .io_write_input_2_exp(input_str_2.io_write_input_exp[2]),
        .io_write_input_2_frac(input_str_2.io_write_input_frac[2]),
        .io_write_input_3_sign(input_str_2.io_write_input_sign[3]),
        .io_write_input_3_exp(input_str_2.io_write_input_exp[3]),
        .io_write_input_3_frac(input_str_2.io_write_input_frac[3]),
        .io_write_input_4_sign(input_str_2.io_write_input_sign[4]),
        .io_write_input_4_exp(input_str_2.io_write_input_exp[4]),
        .io_write_input_4_frac(input_str_2.io_write_input_frac[4]),
        .io_write_input_5_sign(input_str_2.io_write_input_sign[5]),
        .io_write_input_5_exp(input_str_2.io_write_input_exp[5]),
        .io_write_input_5_frac(input_str_2.io_write_input_frac[5]),
        .io_write_input_6_sign(input_str_2.io_write_input_sign[6]),
        .io_write_input_6_exp(input_str_2.io_write_input_exp[6]),
        .io_write_input_6_frac(input_str_2.io_write_input_frac[6]),
        .io_write_input_7_sign(input_str_2.io_write_input_sign[7]),
        .io_write_input_7_exp(input_str_2.io_write_input_exp[7]),
        .io_write_input_7_frac(input_str_2.io_write_input_frac[7]),
        .io_read_req(input_str_2.io_read_req),
        .io_read_column(input_str_2.io_read_column),
        .io_read_row(input_str_2.io_read_row),
        .io_read_output_valid(output_str_2.io_read_output_valid),
        .io_read_output_bits_0_sign(output_str_2.io_read_output_bits_sign[0]),
        .io_read_output_bits_0_exp(output_str_2.io_read_output_bits_exp[0]),
        .io_read_output_bits_0_frac(output_str_2.io_read_output_bits_frac[0]),
        .io_read_output_bits_1_sign(output_str_2.io_read_output_bits_sign[1]),
        .io_read_output_bits_1_exp(output_str_2.io_read_output_bits_exp[1]),
        .io_read_output_bits_1_frac(output_str_2.io_read_output_bits_frac[1]),
        .io_read_output_bits_2_sign(output_str_2.io_read_output_bits_sign[2]),
        .io_read_output_bits_2_exp(output_str_2.io_read_output_bits_exp[2]),
        .io_read_output_bits_2_frac(output_str_2.io_read_output_bits_frac[2]),
        .io_read_output_bits_3_sign(output_str_2.io_read_output_bits_sign[3]),
        .io_read_output_bits_3_exp(output_str_2.io_read_output_bits_exp[3]),
        .io_read_output_bits_3_frac(output_str_2.io_read_output_bits_frac[3]),
        .io_read_output_bits_4_sign(output_str_2.io_read_output_bits_sign[4]),
        .io_read_output_bits_4_exp(output_str_2.io_read_output_bits_exp[4]),
        .io_read_output_bits_4_frac(output_str_2.io_read_output_bits_frac[4]),
        .io_read_output_bits_5_sign(output_str_2.io_read_output_bits_sign[5]),
        .io_read_output_bits_5_exp(output_str_2.io_read_output_bits_exp[5]),
        .io_read_output_bits_5_frac(output_str_2.io_read_output_bits_frac[5]),
        .io_read_output_bits_6_sign(output_str_2.io_read_output_bits_sign[6]),
        .io_read_output_bits_6_exp(output_str_2.io_read_output_bits_exp[6]),
        .io_read_output_bits_6_frac(output_str_2.io_read_output_bits_frac[6]),
        .io_read_output_bits_7_sign(output_str_2.io_read_output_bits_sign[7]),
        .io_read_output_bits_7_exp(output_str_2.io_read_output_bits_exp[7]),
        .io_read_output_bits_7_frac(output_str_2.io_read_output_bits_frac[7]),
        .io_op_valid(input_str_2.io_op_valid),
        .io_op_bits(input_str_2.io_op_bits),
        .io_used_pus_valid(input_str_2.io_used_pus_valid),
        .io_used_pus_bits(input_str_2.io_used_pus_bits),
        .io_lengths_valid(input_str_2.io_lengths_valid),
        .io_lengths_bits_0(input_str_2.io_lengths_bits_0),
        .io_lengths_bits_1(input_str_2.io_lengths_bits_1),
        .io_done(output_str_2.io_done)
    );*/

    // State Machine
    integer i;
    always_ff @(posedge clk) begin
        if(reset) begin
            input_str_0 <= '0;
            input_str_1 <= '0;
            //input_str_2 <= '0;

            fiu.c0Tx.valid <= 1'b0;
            fiu.c1Tx.valid <= 1'b0;

            state <= STATE_WAITING_INPUT;
        end else begin
            if ((state == STATE_WAITING_INPUT) && is_fn_written) begin
                $display("AFU read input U buffer address");
                state <= STATE_IDLE;

                input_head_addr <= byteAddrToClAddr(csrs.cpu_wr_csrs[0].data);
            end else if(state == STATE_IDLE) begin
                input_str_0 <= '0;
                input_str_1 <= '0;
                //input_str_2 <= '0;

                fiu.c0Tx.valid <= 1'b0;
                fiu.c1Tx.valid <= 1'b0;
                if (is_reset_signal_written) begin
                    state <= STATE_RESET;
                end else if (is_start_written) begin
                    state <= STATE_RUN;
                end
            end else if(state == STATE_RUN) begin
                if(cci_c0Rx_isReadRsp(fiu.c0Rx)) begin
                    input_str_0.io_write_req_valid <= fiu.c0Rx.data[0 +: 1];
                    input_str_0.io_write_req_bits <= fiu.c0Rx.data[1 +: 1];
                    input_str_0.io_write_column <= fiu.c0Rx.data[2 +: 5];
                    input_str_0.io_write_row <= fiu.c0Rx.data[7 +: 6];
                    for (i=0; i<8; i++) begin
                        input_str_0.io_write_input_sign[i] <= fiu.c0Rx.data[(13 + i) +: 1];
                        input_str_0.io_write_input_exp[i] <= fiu.c0Rx.data[(14 + i) +: 8];
                        input_str_0.io_write_input_frac[i] <= fiu.c0Rx.data[(22 + i) +: 23];
                    end
                    input_str_0.io_read_req <= fiu.c0Rx.data[45 +: 1];
                    input_str_0.io_read_column <= fiu.c0Rx.data[46 +: 5];
                    input_str_0.io_read_row <= fiu.c0Rx.data[51 +: 6];
                    input_str_0.io_op_valid <= fiu.c0Rx.data[57 +: 1];
                    input_str_0.io_op_bits <= fiu.c0Rx.data[58 +: 2];
                    input_str_0.io_used_pus_valid <= fiu.c0Rx.data[60 +: 1];
                    input_str_0.io_used_pus_bits <= fiu.c0Rx.data[61 +: 1];
                    input_str_0.io_lengths_bits_0 <= fiu.c0Rx.data[63 +: 5];
                    input_str_0.io_lengths_bits_1 <= fiu.c0Rx.data[68 +: 5];

                    input_str_1.io_write_req_valid <= fiu.c0Rx.data[(73 + 0) +: 1];
                    input_str_1.io_write_req_bits <= fiu.c0Rx.data[(73 + 1) +: 1];
                    input_str_1.io_write_column <= fiu.c0Rx.data[(73 + 2) +: 5];
                    input_str_1.io_write_row <= fiu.c0Rx.data[(73 + 7) +: 6];
                    for (i=0; i<8; i++) begin
                        input_str_1.io_write_input_sign[i] <= fiu.c0Rx.data[(90 + i) +: 1];
                        input_str_1.io_write_input_exp[i] <= fiu.c0Rx.data[(91 + i) +: 8];
                        input_str_1.io_write_input_frac[i] <= fiu.c0Rx.data[(99 + i) +: 23];
                    end
                    input_str_1.io_read_req <= fiu.c0Rx.data[(73 + 45) +: 1];
                    input_str_1.io_read_column <= fiu.c0Rx.data[(73 + 46) +: 5];
                    input_str_1.io_read_row <= fiu.c0Rx.data[(73 + 51) +: 6];
                    input_str_1.io_op_valid <= fiu.c0Rx.data[(73 + 57) +: 1];
                    input_str_1.io_op_bits <= fiu.c0Rx.data[(73 + 58) +: 2];
                    input_str_1.io_used_pus_valid <= fiu.c0Rx.data[(73 + 60) +: 1];
                    input_str_1.io_used_pus_bits <= fiu.c0Rx.data[(73 + 61) +: 1];
                    input_str_1.io_lengths_valid <= fiu.c0Rx.data[(73 + 62) +: 1];
                    input_str_1.io_lengths_bits_0 <= fiu.c0Rx.data[(73 + 63) +: 5];
                    input_str_1.io_lengths_bits_1 <= fiu.c0Rx.data[(73 + 68) +: 5];

                    /*input_str_2.io_write_req_valid <= fiu.c0Rx.data[(146 + 0) +: 1];
                    input_str_2.io_write_req_bits <= fiu.c0Rx.data[(146 + 1) +: 1];
                    input_str_2.io_write_column <= fiu.c0Rx.data[(146 + 2) +: 5];
                    input_str_2.io_write_row <= fiu.c0Rx.data[(146 + 7) +: 6];
                    for (i=0; i<8; i++) begin
                        input_str_2.io_write_input_sign[i] <= fiu.c0Rx.data[(163 + i) +: 1];
                        input_str_2.io_write_input_exp[i] <= fiu.c0Rx.data[(164 + i) +: 8];
                        input_str_2.io_write_input_frac[i] <= fiu.c0Rx.data[(172 + i) +: 23];
                    end
                    input_str_2.io_read_req <= fiu.c0Rx.data[(146 + 45) +: 1];
                    input_str_2.io_read_column <= fiu.c0Rx.data[(146 + 46) +: 5];
                    input_str_2.io_read_row <= fiu.c0Rx.data[(146 + 51) +: 6];
                    input_str_2.io_op_valid <= fiu.c0Rx.data[(146 + 57) +: 1];
                    input_str_2.io_op_bits <= fiu.c0Rx.data[(146 + 58) +: 2];
                    input_str_2.io_used_pus_valid <= fiu.c0Rx.data[(146 + 60) +: 1];
                    input_str_2.io_used_pus_bits <= fiu.c0Rx.data[(146 + 61) +: 1];
                    input_str_2.io_lengths_valid <= fiu.c0Rx.data[(146 + 62) +: 1];
                    input_str_2.io_lengths_bits_0 <= fiu.c0Rx.data[(146 + 63) +: 5];
                    input_str_2.io_lengths_bits_1 <= fiu.c0Rx.data[(146 + 68) +: 5];*/

                    state <= STATE_OUTPUT;
                end
            end else if(state == STATE_OUTPUT) begin
                if(cci_c0Rx_isReadRsp(fiu.c0Rx)) begin
                    if (fiu.c0Rx.data[0 +: 4] == 0) begin
                        fiu.c1Tx.data[0 +: 258] <= output_str_0;
                    end else if(fiu.c0Rx.data[0 +: 4] == 1) begin
                        fiu.c1Tx.data[253 +: 258] <= output_str_1;
                    //end else if (fiu.c0Rx.data[0 +: 4] == 2) begin
                    //    fiu.c1Tx.data[0 +: 258] <= output_str_2;
                    end
                end

                state <= STATE_IDLE;
            end else if(state == STATE_RESET) begin 
                $display("Reset Outer Loop PE");
                state <= STATE_IDLE;

                input_str_0 <= '0;
                input_str_1 <= '0;
                //input_str_2 <= '0;

                fiu.c0Tx.valid <= 1'b0;
                fiu.c1Tx.valid <= 1'b0;
            end else begin
                input_str_0 <= '0;
                input_str_1 <= '0;
                //input_str_2 <= '0;

                fiu.c0Tx.valid <= 1'b0;
                fiu.c1Tx.valid <= 1'b0;
            end
        end
    end

    assign fiu.c2Tx.mmioRdValid = 1'b0;

endmodule