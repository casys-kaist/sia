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

    // TODO: CSR write handling variables
    logic is_fn_written;
    assign is_fn_written = csrs.cpu_wr_csrs[0].en;

    // =========================================================================
    //   Main AFU logic
    // =========================================================================
    // TODO: Define FSM states
    typedef enum logic [3:0] {
        STATE_WAITING_INPUT,
        STATE_WAITING_OUTPUT,
        STATE_IDLE,

        STATE_REQUEST,
        STATE_OP,
        STATE_WAIT1,
        STATE_WAIT2,
        STATE_RESPONSE,

        STATE_RESET
    } t_state;
    t_state state;

    t_ccip_clAddr input_addr;
    t_ccip_clAddr output_addr;

    // TODO: Input buffer Read Header
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

    // TODO: Output buffer Write Header
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

    // TODO: DUT
    logic br_reset;                     //  i

    // TODO: State Machine
    always_ff @(posedge clk) begin
        if(reset) begin
            input_addr <= t_cci_clAddr'(0);
            output_addr <= t_cci_clAddr'(0);

            fiu.c0Tx.valid <= 1'b0;
            fiu.c1Tx.valid <= 1'b0;

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
            end else begin
                fiu.c0Tx.valid <= 1'b0;
                fiu.c1Tx.valid <= 1'b0;

            end
        end
    end

    assign fiu.c2Tx.mmioRdValid = 1'b0;

endmodule