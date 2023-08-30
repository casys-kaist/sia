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

    // =========================================================================
    //   Main AFU logic
    // =========================================================================
    typedef enum logic [3:0] {
        STATE_WAITING_INPUT,
        STATE_WAITING_OUTPUT,
        STATE_IDLE,

        STATE_REQUEST,
        STATE_OP,
        STATE_WAIT,
        STATE_RESPONSE,

        STATE_RESET
    } t_state;
    t_state state;

    logic [7:0] waiting_counter;
    logic [1:0] current_function;

    t_ccip_clAddr input_addr;
    t_ccip_clAddr output_addr;

    // Input buffer Read Header
    t_cci_mpf_c0_ReqMemHdr input_buffer_read_hdr;
    t_cci_mpf_ReqMemHdrParams input_buffer_read_params;
    always_comb begin
        input_buffer_read_params = cci_mpf_defaultReqHdrParams(1);
        input_buffer_read_params.vc_sel = eVC_VL0;
        input_buffer_read_params.cl_len = eCL_LEN_1;
        input_buffer_read_hdr = cci_mpf_c0_genReqHdr(eREQ_RDLINE_I,
                                                     input_addr,
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
                                                       output_addr,
                                                       t_cci_mdata'(0),
                                                       output_buffer_write_params);
    end

    // DUT
    // !! These signals will be shard across multiple float modules !!
    logic fa_reset;                     //  i
    logic fa_a_sign;                    //  i
    logic [7:0] fa_a_exp;               //  i
    logic [22:0] fa_a_frac;             //  i
    logic fa_b_sign;                    //  i   \
    logic [7:0] fa_b_exp;               //  i   | Won't be used for single input modules
    logic [22:0] fa_b_frac;             //  i   /

    // Adder output signal
    logic fa_out_sign;                  //o
    logic [7:0] fa_out_exp;             //o
    logic [22:0] fa_out_frac;           //o

    // Multiplier output signal
    logic fm_out_sign;                  //o
    logic [7:0] fm_out_exp;             //o
    logic [22:0] fm_out_frac;           //o

    // Divider output signal
    logic fd_out_sign;                  //o
    logic [7:0] fd_out_exp;             //o
    logic [22:0] fd_out_frac;           //o

    // Sqrt output signal
    logic fs_out_sign;                  //o
    logic [7:0] fs_out_exp;             //o
    logic [22:0] fs_out_frac;           //o

    assign fa_reset = (reset || (state == STATE_RESET));

    FloatAdd float_add_unit (
        .clock(clk),
        .reset(fa_reset),
        .io_input_a_sign(fa_a_sign),
        .io_input_a_exp(fa_a_exp),
        .io_input_a_frac(fa_a_frac),
        .io_input_b_sign(fa_b_sign),
        .io_input_b_exp(fa_b_exp),
        .io_input_b_frac(fa_b_frac),
        .io_output_sign(fa_out_sign),
        .io_output_exp(fa_out_exp),
        .io_output_frac(fa_out_frac)
    );

    FloatMult float_mult_unit (
        .clock(clk),
        .reset(fa_reset),
        .io_input_a_sign(fa_a_sign),
        .io_input_a_exp(fa_a_exp),
        .io_input_a_frac(fa_a_frac),
        .io_input_b_sign(fa_b_sign),
        .io_input_b_exp(fa_b_exp),
        .io_input_b_frac(fa_b_frac),
        .io_output_sign(fm_out_sign),
        .io_output_exp(fm_out_exp),
        .io_output_frac(fm_out_frac)
    );

    FloatOneDiv float_twodiv_unit (
        .clock(clk),
        .reset(fa_reset),
        .io_input_sign(fa_a_sign),
        .io_input_exp(fa_a_exp),
        .io_input_frac(fa_a_frac),
        .io_output_sign(fd_out_sign),
        .io_output_exp(fd_out_exp),
        .io_output_frac(fd_out_frac)
    );

    FloatSqrt float_sqrt_unit (
        .clock(clk),
        .reset(fa_reset),
        .io_input_sign(fa_a_sign),
        .io_input_exp(fa_a_exp),
        .io_input_frac(fa_a_frac),
        .io_output_sign(fs_out_sign),
        .io_output_exp(fs_out_exp),
        .io_output_frac(fs_out_frac)
    );

    // State Machine
    always_ff @(posedge clk) begin
        if(reset) begin
            input_addr <= t_cci_clAddr'(0);
            output_addr <= t_cci_clAddr'(0);

            waiting_counter <= 8'b0;
            current_function <= 2'b0;

            fiu.c0Tx.valid <= 1'b0;
            fiu.c1Tx.valid <= 1'b0;

            fa_a_sign <= 1'b0;
            fa_a_exp <= 8'b0;
            fa_a_frac <= 23'b0;
            fa_b_sign <= 1'b0;
            fa_b_exp <= 8'b0;
            fa_b_frac <= 23'b0;

            state <= STATE_WAITING_INPUT;
        end else begin
            if((state == STATE_WAITING_INPUT) && is_input_buf_written) begin
                state <= STATE_WAITING_OUTPUT;
                $display("AFU read Input buffer address");

                input_addr <= byteAddrToClAddr(csrs.cpu_wr_csrs[1].data);
            end else if((state == STATE_WAITING_OUTPUT) && is_output_buf_written) begin
                state <= STATE_IDLE;
                $display("AFU read Output buffer address");

                output_addr <= byteAddrToClAddr(csrs.cpu_wr_csrs[2].data);
            end else if(state == STATE_IDLE) begin
                fiu.c0Tx.valid <= 1'b0;
                fiu.c1Tx.valid <= 1'b0;

                waiting_counter <= 8'b0;
                current_function <= 2'b0;

                if(is_fn_written) begin
                    $display("AFU got start signal, send it to float modules");
                    current_function <= csrs.cpu_wr_csrs[0].data[1:0];
                    state <= STATE_REQUEST;
                end else if(is_reset_signal_written) begin
                    $display("AFU got reset signal, send it to float modules");
                    state <= STATE_RESET;
                end else begin
                    state <= STATE_IDLE;
                end
            end else if(state == STATE_REQUEST) begin
                $display("AFU sent input buffer read request");
                state <= STATE_OP;
                
                fiu.c0Tx.valid <= 1'b1;
                fiu.c0Tx.hdr <= input_buffer_read_hdr;
            end else if(state == STATE_OP) begin
                fiu.c0Tx.valid <= 1'b0;
                if(cci_c0Rx_isReadRsp(fiu.c0Rx)) begin
                    $display("AFU got input buffer read response");
                    $display("input_a_sign(%b), input_a_exp(%b), input_a_frac(%b), input_b_sign(%b), input_b_exp(%b), input_b_frac(%b)",
                            fiu.c0Rx.data[31:31],
                            fiu.c0Rx.data[30:23],
                            fiu.c0Rx.data[22:0],
                            fiu.c0Rx.data[63:63],
                            fiu.c0Rx.data[62:55],
                            fiu.c0Rx.data[54:32]);
                    state <= STATE_WAIT;

                    fa_a_sign <= fiu.c0Rx.data[31:31];
                    fa_a_exp <= fiu.c0Rx.data[30:23];
                    fa_a_frac <= fiu.c0Rx.data[22:0];
                    fa_b_sign <= fiu.c0Rx.data[63:63];
                    fa_b_exp <= fiu.c0Rx.data[62:55];
                    fa_b_frac <= fiu.c0Rx.data[54:32];

                    case (current_function)
                        2'b00: waiting_counter <= 8'd2;
                        2'b01: waiting_counter <= 8'd2;
                        2'b10: waiting_counter <= 8'd25;
                        2'b11: waiting_counter <= 8'd2;
                    endcase
                end
            end else if(state == STATE_WAIT) begin
                $display("Wait for %d cycle", waiting_counter[7:0]);
                waiting_counter <= waiting_counter - 8'b1;
                if (waiting_counter == 8'b0) begin
                    state <= STATE_RESPONSE;
                end else begin
                    state <= STATE_WAIT;
                end
                    
                fa_a_sign <= 1'b0;
                fa_a_exp <= 8'b0;
                fa_a_frac <= 23'b0;
                fa_b_sign <= 1'b0;
                fa_b_exp <= 8'b0;
                fa_b_frac <= 23'b0;
            end else if(state == STATE_RESPONSE) begin
                $display("AFU sent output buffer write request");
                case (current_function)
                    2'b00: begin
                        $display("adder: output_sign(%b), output_exp(%b), output_frac(%b)", fa_out_sign, fa_out_exp, fa_out_frac);
                        fiu.c1Tx.data <= t_ccip_clData'({448'b0, fa_out_sign, fa_out_exp, fa_out_frac, 32'b1});
                    end
                    2'b01: begin
                        $display("multiplier: output_sign(%b), output_exp(%b), output_frac(%b)", fm_out_sign, fm_out_exp, fm_out_frac);
                        fiu.c1Tx.data <= t_ccip_clData'({448'b0, fm_out_sign, fm_out_exp, fm_out_frac, 32'b1});
                    end
                    2'b10: begin
                        $display("divider: output_sign(%b), output_exp(%b), output_frac(%b)", fd_out_sign, fd_out_exp, fd_out_frac);
                        fiu.c1Tx.data <= t_ccip_clData'({448'b0, fd_out_sign, fd_out_exp, fd_out_frac, 32'b1});
                    end
                    2'b11: begin
                        $display("sqrt: output_sign(%b), output_exp(%b), output_frac(%b)", fs_out_sign, fs_out_exp, fs_out_frac);
                        fiu.c1Tx.data <= t_ccip_clData'({448'b0, fs_out_sign, fs_out_exp, fs_out_frac, 32'b1});
                    end
                endcase
                state <= STATE_IDLE;

                fiu.c1Tx.valid <= 1'b1;
                fiu.c1Tx.hdr <= output_buffer_write_hdr;
            end else if(state == STATE_RESET) begin
                $display("Reset bus_read");
                state <= STATE_IDLE;
            end else begin
                fiu.c0Tx.valid <= 1'b0;
                fiu.c1Tx.valid <= 1'b0;

                fa_a_sign <= 1'b0;
                fa_a_exp <= 8'b0;
                fa_a_frac <= 23'b0;
                fa_b_sign <= 1'b0;
                fa_b_exp <= 8'b0;
                fa_b_frac <= 23'b0;

                waiting_counter <= 8'b0;
                current_function <= 2'b0;
            end
        end
    end

    assign fiu.c2Tx.mmioRdValid = 1'b0;

endmodule