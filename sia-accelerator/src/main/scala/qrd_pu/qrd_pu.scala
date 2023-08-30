package qrd_pu

import chisel3._
import chisel3.util._
import floats.FloatType
import qrd_pe.OuterLoopPE
import qrd_pe.InnerLoopPE

/**
 * QRD_PU: PU for the QRD unit that calculates R matrix for a small matrix
 *      - op: Trigger calculation
 *      - length: Number of rows of the matrix (x NUM_DOT_PE)
 *      - done: Signal indicating module operation done
 *      
 *      - write_req: Matrix buffer write request
 *      - write_column: Column number of input vector
 *      - write_row: Row number of the start of input vector (write_row ~ write_row + NUM_DOT_PE)
 *      - input: A vector of input matrix elements (NUM_DOT_PE granulity)
 * 
 *      - read_req: R matrix buffer read request
 *      - read_column: Column number of the start of output vector (read_column ~ read_column + NUM_DOT_PE)
 *      - read_row: Row number of output vector 
 *      - output: A vector of R matrix elements (NUM_DOT_PE granulity)
 * 
 * @param KEY_LENGTH: Key length of learned index (Row/Column size of R matrix)
 * @param PE_NUM: Number of PEs per a PU
 * @param NUM_DOT_PE: Number of parallel multipliers at dot product tree unit
 * @param MEM_SIZE: Maximum depth of the memory
 * @param QUEUE_SIZE: Maximum depth of the queue
 */

class QRD_PU(PE_NUM: Int, KEY_LENGTH: Int, NUM_DOT_PE: Int, MEM_SIZE: Int, QUEUE_SIZE: Int) extends Module {
    val io = IO(new Bundle {
        val op = Input(Bool())
        val length = Flipped(new Valid(UInt(log2Ceil(MEM_SIZE).W)))
        val done = Output(Bool())

        val write_req = Input(Bool())
        val write_column = Input(UInt(log2Ceil(KEY_LENGTH).W))
        val write_row = Input(UInt(log2Ceil(MEM_SIZE).W))
        val input = Input(Vec(NUM_DOT_PE, new FloatType(8, 23)))

        val read_req = Input(Bool())
        val read_column = Input(UInt(log2Ceil(KEY_LENGTH).W))
        val read_row = Input(UInt(log2Ceil(KEY_LENGTH).W))
        val output = new Valid(Vec(NUM_DOT_PE, new FloatType(8, 23)))
    })

    // Modules
    val matrix_buffer = Module(new Matrix_Buffer_Wrapper(KEY_LENGTH, PE_NUM, NUM_DOT_PE, MEM_SIZE))
        // Input - (NUM_DOT_PE granulity) x PE_NUM
        // Output - (NUM_DOT_PE granulity) x PE_NUM
    val scalar_register = Module(new Scalar_Register())
        // Input - A value
        // Output - A value
    val reflector_buffer = Module(new Reflector_Buffer(NUM_DOT_PE, MEM_SIZE))
        // Input - (NUM_DOT_PE granulity)
        // Output - (NUM_DOT_PE granulity)
    val r_matrix_buffer = Module(new R_Matrix_Buffer_Wrapper(KEY_LENGTH, PE_NUM, NUM_DOT_PE))
        // Input - (A value) x PE_NUM
        // Output - (NUM_DOT_PE granulity)

    val outer_loop_pe = Module(new OuterLoopPE(NUM_DOT_PE, QUEUE_SIZE))
    val inner_loop_pe_vec = VecInit(Seq.fill(PE_NUM) {
        Module(new InnerLoopPE(NUM_DOT_PE, QUEUE_SIZE)).io
    })

    // States for calcution FSM
    val calc_IDLE :: calc_LOAD_OUTER :: calc_OUTER :: calc_LOAD_INNER :: calc_INNER :: calc_FINISH :: Nil = Enum(6)
    val state_reg = RegInit(calc_IDLE)
    val length_reg = RegInit(0.U(log2Ceil(MEM_SIZE).W))
    when (io.length.valid) { length_reg := io.length.bits }

    /***************************
     * Initialize output ports *
     ***************************/
    io.done := false.B
    io.output.valid := false.B
    io.output.bits := VecInit(Seq.fill(NUM_DOT_PE)(0.U.asTypeOf(new FloatType(8, 23))))

    /***************************
     * Initialize module ports *
     ***************************/
    matrix_buffer.io.write_req := VecInit(Seq.fill(PE_NUM)(false.B))
    matrix_buffer.io.write_column := VecInit((0 until PE_NUM).map(_.U).toSeq)
    matrix_buffer.io.write_row := VecInit(Seq.fill(PE_NUM)(0.U))
    matrix_buffer.io.input := VecInit(Seq.fill(PE_NUM)(
        VecInit(Seq.fill(NUM_DOT_PE)(0.U.asTypeOf(new FloatType(8, 23))))
    ))
    matrix_buffer.io.read_req := VecInit(Seq.fill(PE_NUM)(false.B))
    matrix_buffer.io.read_column := VecInit((0 until PE_NUM).map(_.U).toSeq)
    matrix_buffer.io.read_row := VecInit(Seq.fill(PE_NUM)(0.U))

    scalar_register.io.reset := false.B
    scalar_register.io.input.valid := outer_loop_pe.io.output_gamma.valid
    scalar_register.io.input.bits := outer_loop_pe.io.output_gamma.bits

    reflector_buffer.io.request := false.B
    reflector_buffer.io.input.valid := outer_loop_pe.io.output_reflector.valid
    reflector_buffer.io.input.bits := outer_loop_pe.io.output_reflector.bits
    
    r_matrix_buffer.io.write_req := VecInit(Seq.fill(PE_NUM)(false.B))
    r_matrix_buffer.io.write_column := VecInit((0 until PE_NUM).map(_.U).toSeq)
    r_matrix_buffer.io.write_row := VecInit(Seq.fill(PE_NUM)(0.U))
    r_matrix_buffer.io.input := VecInit(Seq.fill(PE_NUM)(0.U.asTypeOf(new FloatType(8, 23))))
    r_matrix_buffer.io.read_req := VecInit(Seq.fill(NUM_DOT_PE)(false.B))
    r_matrix_buffer.io.read_column := VecInit((0 until NUM_DOT_PE).map(_.U).toSeq)
    r_matrix_buffer.io.read_row := VecInit(Seq.fill(NUM_DOT_PE)(0.U))
    
    outer_loop_pe.io.start.valid := false.B
    outer_loop_pe.io.start.bits := 0.U
    outer_loop_pe.io.input.valid := false.B
    outer_loop_pe.io.input.bits := VecInit(Seq.fill(NUM_DOT_PE)(0.U.asTypeOf(new FloatType(8, 23))))

    for (i <- 0 until PE_NUM) {
        inner_loop_pe_vec(i).start.valid := false.B
        inner_loop_pe_vec(i).start.bits := 0.U
        inner_loop_pe_vec(i).input_u.valid := false.B
        inner_loop_pe_vec(i).input_u.bits := VecInit(Seq.fill(NUM_DOT_PE)(0.U.asTypeOf(new FloatType(8, 23))))
        inner_loop_pe_vec(i).input_a.valid := false.B
        inner_loop_pe_vec(i).input_a.bits := VecInit(Seq.fill(NUM_DOT_PE)(0.U.asTypeOf(new FloatType(8, 23))))
        inner_loop_pe_vec(i).input_gamma.valid := false.B
        inner_loop_pe_vec(i).input_gamma.bits := 0.U.asTypeOf(new FloatType(8, 23))
        inner_loop_pe_vec(i).reset := false.B
    }

    /*********************************
      * Logic for input matrix write *
      ********************************/ 
    when(io.write_req === true.B & (state_reg === calc_IDLE)) {
        matrix_buffer.io.write_req(0) := true.B
        matrix_buffer.io.write_column(0) := io.write_column
        matrix_buffer.io.write_row(0) := io.write_row
        matrix_buffer.io.input(0) := io.input
    }

    /***************************
     * Logic for R matrix read *
     ***************************/
    val output_valid_reg = RegInit(false.B)
    when(io.read_req === true.B & (state_reg === calc_IDLE)) {
        for (i <- 0 until NUM_DOT_PE) {
            r_matrix_buffer.io.read_req(i) := true.B
            r_matrix_buffer.io.read_column(i) := io.read_column + i.U
            r_matrix_buffer.io.read_row(i) := io.read_row
        }
        output_valid_reg := true.B
    } .otherwise {
        output_valid_reg := false.B
    }
    io.output.valid := output_valid_reg
    io.output.bits := r_matrix_buffer.io.output

    /*****************************
     * Logic for QRD calculation *
     *****************************/
    val outer_current_column = RegInit(0.U(log2Ceil(KEY_LENGTH).W)) // Column # of current vector
    val outer_current_row = RegInit(0.U(log2Ceil(MEM_SIZE).W))      // Start row # of current vector (length of NUM_DOT_PE)

    val inner_current_column = RegInit(0.U(log2Ceil(KEY_LENGTH).W)) // Column # of current vector    (length of PE_NUM)
    val inner_current_row = RegInit(0.U(log2Ceil(MEM_SIZE).W))      // Start row # of current vector (length of NUM_DOT_PE)

    /** ** Workflow for QRD calculation ***
        1. Load selected column to the outer loop PE
        2. Calculate reflector from the selected column with outer loop PE
        3. Store reflector to the Reflector buffer & Store gamma to the scalar register
        4. Load selected column and its right columns to the inner loop PEs
        5. Apply reflector and gamma with outer loop PE
        6. Store result columns to the matrix buffer & Store R matrix element to the R matrix buffer
        7. Select the next column and go back to 2

     *  calc_IDLE: Waiting for the signal
     *  calc_LOAD_OUTER: Load target column to the outer loop PE
     *  calc_OUTER: Outer loop PE calculates the reflector
     *              - Waiting for the output
     *              - Valid outputs are stored to the reflector buffer & scalar register
     *  calc_LOAD_INNER: Load target columns to the inner loop PEs
     *  calc_INNER: Inner loop PEs apply the reflector & calc R matrix
     *              - Waiting for the output
     *              - Valid outputs are stored to the matrix buffer & R matrix buffer
     *              - Repeat from the calc_LOAD_INNER if all the columns are not processed yet
     *              - Repeat from the calc_LOAD_OUTER if all the columns are processed
     *              - Transit to calc_RETURN if this is the last column
     *  calc_FINISH: Emit done signal
     */

    // Registers for pipelining memory outputs
    val outer_valid_reg = RegInit(false.B)
    val outer_input_row_reg = RegInit(0.U(log2Ceil(MEM_SIZE).W)) // Register for matrix_buffer -> outer PE
    val inner_valid_reg = RegInit(false.B)
    val inner_input_row_reg = RegInit(0.U(log2Ceil(MEM_SIZE).W)) // Register for matrix_buffer -> inner PE

    val inner_output_row_reg = RegInit(0.U(log2Ceil(MEM_SIZE).W)) // Counter for innner PE -> matrix buffer

    val load_outer_start_reg = RegInit(false.B)
    val load_inner_start_reg = RegInit(false.B)

    val cycle_counter = RegInit(0.U(3.W))

    switch (state_reg) {
        is (calc_IDLE) {
            // State transition
            when (io.op === true.B) {
                assert(length_reg != 0.U)
                load_outer_start_reg := true.B
                outer_current_column := 0.U
                outer_current_row := 0.U
                
                cycle_counter := 0.U

                state_reg := calc_LOAD_OUTER
            }
        }
        is (calc_LOAD_OUTER) {
            when (load_outer_start_reg === true.B) {
                outer_loop_pe.io.start.valid := true.B
                outer_loop_pe.io.start.bits := length_reg // FIXME: Need to consider the case KEY_LENGTH % PE_NUM != 0
                load_outer_start_reg := false.B
            }

            // First cycle (Request for the matrix buffer output)
            when (cycle_counter === 0.U) {
                matrix_buffer.io.read_req(0) := true.B
                matrix_buffer.io.read_column(0) := outer_current_column
                matrix_buffer.io.read_row(0) := outer_current_row
                outer_valid_reg := true.B
                outer_input_row_reg := outer_current_row

                cycle_counter := 4.U
                outer_current_row := outer_current_row + 1.U
            } .otherwise {
                matrix_buffer.io.read_req(0) := false.B
                matrix_buffer.io.read_column(0) := 0.U
                matrix_buffer.io.read_row(0) := 0.U
                outer_valid_reg := false.B
                outer_input_row_reg := outer_current_row

                cycle_counter := cycle_counter - 1.U;
            }

            // Second cycle (Receive the matrix buffer output & Feed it to the outer loop PE)

            // TODO: Skip input vector only filled with zeros
            outer_loop_pe.io.input.valid := outer_valid_reg
            for (i <- 0 until NUM_DOT_PE) {
                outer_loop_pe.io.input.bits(i) := Mux( (((outer_input_row_reg << log2Ceil(NUM_DOT_PE).U) + i.U) > outer_current_column),
                    matrix_buffer.io.output(0)(i), 0.U.asTypeOf(new FloatType(8, 23))
                )
            }

            // State transition
            when (outer_input_row_reg === length_reg) {
                state_reg := calc_OUTER
            }
        }
        is (calc_OUTER) {
            // Waiting for the done signal
            // Outer loop PE outputs are redirected to reflector buffer & scalar register
            when(outer_loop_pe.io.done === true.B) {
                load_inner_start_reg := true.B
                inner_current_column := outer_current_column
                inner_current_row := 0.U
                inner_input_row_reg := 0.U
                inner_output_row_reg := 0.U

                cycle_counter := 0.U

                for (i <- 0 until PE_NUM) { inner_loop_pe_vec(i).reset := true.B }

                state_reg := calc_LOAD_INNER
            }
        }
        is (calc_LOAD_INNER) {            
            // FIXME: Need to consider the case KEY_LENGTH % PE_NUM != 0
            for (i <- 0 until PE_NUM) {
                when (load_inner_start_reg === true.B) {
                    inner_loop_pe_vec(i).start.valid := true.B
                    inner_loop_pe_vec(i).start.bits := length_reg // FIXME: Need to consider the case KEY_LENGTH % PE_NUM != 0
                    load_inner_start_reg := false.B
                    reflector_buffer.io.request := true.B
                }

                // First cycle (Request for the reflector buffer output & matrix buffer output)
                when (cycle_counter === 0.U) {
                    matrix_buffer.io.read_req(i) := true.B
                    matrix_buffer.io.read_column(i) := inner_current_column + i.U
                    matrix_buffer.io.read_row(i) := inner_current_row
                    inner_valid_reg := true.B
                    inner_input_row_reg := inner_current_row

                    cycle_counter := 4.U
                    inner_current_row := inner_current_row + 1.U
                } .otherwise {
                    matrix_buffer.io.read_req(i) := false.B
                    matrix_buffer.io.read_column(i) := 0.U
                    matrix_buffer.io.read_row(i) := 0.U
                    inner_valid_reg := false.B
                    inner_input_row_reg := inner_current_row
                    
                    cycle_counter := cycle_counter - 1.U
                }

                // Second cycle (Receive the outputs & Feed it to the inner loop PE)
                inner_loop_pe_vec(i).input_a.valid := inner_valid_reg
                for (j <- 0 until NUM_DOT_PE) {
                    inner_loop_pe_vec(i).input_a.bits(j) := Mux( (((inner_input_row_reg << log2Ceil(NUM_DOT_PE).U) + i.U) > outer_current_column),
                        matrix_buffer.io.output(i)(j), 0.U.asTypeOf(new FloatType(8, 23))
                    ) 
                }
                inner_loop_pe_vec(i).input_u.valid := reflector_buffer.io.output.valid
                inner_loop_pe_vec(i).input_u.bits := reflector_buffer.io.output.bits

                inner_loop_pe_vec(i).input_gamma := scalar_register.io.output
            }
            // State transition
            when (inner_input_row_reg === length_reg) {
                state_reg := calc_INNER
            }
        }
        is (calc_INNER) {
            when (inner_loop_pe_vec(0).done === true.B) {
                for (i <- 1 until PE_NUM) { assert(inner_loop_pe_vec(i).done === true.B) }
                // Determine the next state
                when (PE_NUM.U.asTypeOf(UInt((log2Ceil(KEY_LENGTH) + 1).W)) + inner_current_column.asTypeOf(UInt((log2Ceil(KEY_LENGTH) + 1).W)) < KEY_LENGTH.U((log2Ceil(KEY_LENGTH) + 1).W)) {
                    load_inner_start_reg := true.B
                    inner_current_column := inner_current_column + PE_NUM.U
                    inner_current_row := 0.U
                    inner_input_row_reg := 0.U
                    inner_output_row_reg := 0.U

                    cycle_counter := 0.U

                    for (i <- 0 until PE_NUM) { inner_loop_pe_vec(i).reset := true.B }

                    state_reg := calc_LOAD_INNER
                } .elsewhen (outer_current_column.asTypeOf(UInt((log2Ceil(KEY_LENGTH) + 1).W)) + 1.U === KEY_LENGTH.U((log2Ceil(KEY_LENGTH) + 1).W)) {
                    state_reg := calc_FINISH
                } .otherwise {
                    load_outer_start_reg := true.B
                    outer_current_column := outer_current_column + 1.U
                    outer_current_row := 0.U
                    outer_input_row_reg := 0.U

                    cycle_counter := 0.U

                    outer_loop_pe.reset := true.B
                    scalar_register.reset := true.B
                    reflector_buffer.reset := true.B

                    state_reg := calc_LOAD_OUTER
                }
            }
        }
        is (calc_FINISH) {
            state_reg := calc_IDLE

            for (i <- 0 until PE_NUM) { inner_loop_pe_vec(i).reset := true.B }
            outer_loop_pe.reset := true.B
            scalar_register.reset := true.B
            reflector_buffer.reset := true.B

            io.done := true.B
        }
    }

    when ((state_reg === calc_LOAD_INNER | state_reg === calc_INNER) & (inner_loop_pe_vec(0).output.valid === true.B)) {
        for (i <- 0 until PE_NUM) { // FIXME: Need to consider the case KEY_LENGTH % PE_NUM != 0
            assert(inner_loop_pe_vec(i).output.valid === true.B)

            /********************************************
             * Logic for inner loop PE -> matrix buffer *
             ********************************************/
            matrix_buffer.io.write_req(i) := true.B
            matrix_buffer.io.write_column(i) := inner_current_column + i.U
            matrix_buffer.io.write_row(i) := inner_output_row_reg
            matrix_buffer.io.input(i) := inner_loop_pe_vec(i).output.bits
            
            /**********************************************
             * Logic for inner loop PE -> R matrix buffer *
             **********************************************/
            when ((inner_current_column >> log2Ceil(NUM_DOT_PE).U) === inner_output_row_reg) {
                r_matrix_buffer.io.write_req(i) := true.B
                r_matrix_buffer.io.write_column(i) := inner_current_column + i.U
                r_matrix_buffer.io.write_row(i) := inner_current_column - (inner_output_row_reg << log2Ceil(NUM_DOT_PE).U)
                r_matrix_buffer.io.input(i) := inner_loop_pe_vec(i).output.bits(inner_current_column(2, 0))
            }
        }
        
        inner_output_row_reg := inner_output_row_reg + 1.U
    }
}