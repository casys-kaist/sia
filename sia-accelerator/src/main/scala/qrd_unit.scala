import chisel3._
import chisel3.util._
import floats.FloatType
import qrd_pu.QRD_PU
import qrd_pu.Matrix_Buffer_Wrapper

/**
  * QRD_Unit: Entire training element that calculates R matrix of the given matrix
  *     - write_req: Write input matrix to the given number of PU
  *         - valid: Write request
  *         - bits: Target PU number
  *     - write_column: Column number of input vector
  *     - write_row: Row number of the start of input vector (write_row ~ write_row + NUM_DOT_PE)
  *         - Write addresses are per-PU addresses
  *     - write_input: A vector of input_matrix elements (NUM_DOT_PE granulity)
  *
  *     - read_req: Result matrix buffer read request
  *     - read_column: Column number of output vector
  *     - read_row: Row number of the start of output vector (read_row ~ read_row + NUM_DOT_PE)
  *         - Read addresses are global addresses
  *     - read_output: A vector of output_matrix elements (NUM_DOT_PE granulity)
  * 
  *     - op: Trigger calculaton & Select operation mode
  *         - 0: NOP
  *         - 1: Use matrix in the PUs
  *         - 2: Redirect matrix in the result buffer (for parallel QRD)
  *         - 3: Do entire parallel QRD (1 -> 2 -> 2 -> ...)
  *     - used_pus: Number of PUs that will be used for calculation
  *     - lengths: Number of rows of the matrix of each PU (x NUM_DOT_PE)
  *     - done: Signal indicating module operation done
  * 
  * @param PE_NUM: Number of PEs per a PU
  * @param PU_NUM: Total number of PUs
  * @param KEY_LENGTH: Key length of learned index (Row/Column size of R matrix)
  * @param NUM_DOT_PE: Number of parallel multipliers at dot product tree unit
  * @param MEM_SIZE: Maximum depth of the memory
  * @param QUEUE_SIZE: Maximum depth of the queue
  */

class QRD_Unit(
    PU_NUM: Int,
    PE_NUM: Int,
    KEY_LENGTH: Int,
    NUM_DOT_PE: Int,
    MEM_SIZE: Int,
    QUEUE_SIZE: Int
) extends Module {
    val io = IO(new Bundle{
        val write_req = Flipped(new Valid(UInt(log2Ceil(PU_NUM).max(1).W)))
        val write_column = Input(UInt(log2Ceil(KEY_LENGTH).W))
        val write_row = Input(UInt(log2Ceil(MEM_SIZE).W))
        val write_input = Input(Vec(NUM_DOT_PE, new FloatType(8, 23)))

        val read_req = Input(Bool())
        val read_column = Input(UInt(log2Ceil(KEY_LENGTH).W))
        val read_row = Input(UInt(log2Ceil(KEY_LENGTH * PU_NUM).W))
        val read_output = new Valid(Vec(NUM_DOT_PE, new FloatType(8, 23)))

        val op = Flipped(new Valid(UInt(2.W)))
        val used_pus = Flipped(new Valid(UInt(log2Ceil(PU_NUM).max(1).W)))
        val lengths = Flipped(new Valid(Vec(PU_NUM, UInt(log2Ceil(MEM_SIZE).W))))
        val done = Output(Bool())
    })
    assert(KEY_LENGTH % NUM_DOT_PE == 0) // TODO: Support the case KEY_LEGNTH % NUM_DOT_PE != 0

    /*************
     *  Modules  *
     *************/
    val qrd_output_buffer = Module(new QRD_Output_Buffer(PU_NUM, KEY_LENGTH, NUM_DOT_PE))
    val pu_vec = Seq.fill(PU_NUM)(
        Module(new QRD_PU(PE_NUM, KEY_LENGTH, NUM_DOT_PE, MEM_SIZE, QUEUE_SIZE))
    )
    
    /******************
     * States for FSM *
     ******************/ 
    val state_IDLE :: state_INPUT :: state_TRIGGER :: state_WAITING :: state_OUTPUT :: state_DONE :: Nil = Enum(6)
    val state_reg = RegInit(state_IDLE)
    val current_op = RegInit(0.U(2.W))
    val current_pus = RegInit(0.U(log2Ceil(PU_NUM).max(1).W))

    /***************************
     * Initialize output ports *
     ***************************/
    io.done := false.B

    /***************************
     * Initialize module ports *
     ***************************/
    qrd_output_buffer.io.write_req := VecInit(Seq.fill(PU_NUM)(false.B))
    qrd_output_buffer.io.write_column := VecInit(Seq.fill(PU_NUM)(0.U))
    qrd_output_buffer.io.write_row := VecInit(Seq.fill(PU_NUM)(0.U))
    qrd_output_buffer.io.write_input := VecInit(Seq.fill(PU_NUM)(
        VecInit(Seq.fill(NUM_DOT_PE)(0.U.asTypeOf(new FloatType(8, 23))))
    ))

    qrd_output_buffer.io.read_req := false.B
    qrd_output_buffer.io.read_column := 0.U
    qrd_output_buffer.io.read_row := 0.U

    for (i <- 0 until PU_NUM) {
        pu_vec(i).io.write_req := false.B
        pu_vec(i).io.write_column := 0.U
        pu_vec(i).io.write_row := 0.U
        pu_vec(i).io.input := VecInit(Seq.fill(NUM_DOT_PE)(0.U.asTypeOf(new FloatType(8, 23))))

        pu_vec(i).io.op := false.B
        pu_vec(i).io.read_req := false.B
        pu_vec(i).io.read_column := 0.U
        pu_vec(i).io.read_row := 0.U
    }

    /*************************
     * Logic for setting PUs *
     *************************/
    when (io.used_pus.valid) { current_pus := io.used_pus.bits }
    
    for (i <- 0 until PU_NUM) {
        when (io.lengths.valid) {
            pu_vec(i).io.length.valid := io.lengths.bits(i) =/= 0.U
            pu_vec(i).io.length.bits := io.lengths.bits(i)
        } .otherwise {
            pu_vec(i).io.length.valid := false.B
            pu_vec(i).io.length.bits := 0.U
        }
    }

    /********************************
     * Logic for input matrix write *
     ********************************/
    when(state_reg === state_IDLE) {
        for (i <- 0 until PU_NUM) {
            when((io.write_req.valid === true.B) && (io.write_req.bits === i.U)) {
                pu_vec(i).io.write_req := true.B
                pu_vec(i).io.write_column := io.write_column
                pu_vec(i).io.write_row := io.write_row
                pu_vec(i).io.input := io.write_input
            }
        }
    }

    /********************************
     * Logic for result matrix read *
     ********************************/
    val read_output_valid_reg = RegInit(false.B)
    when((io.read_req === true.B) && (state_reg === state_IDLE)) {
        qrd_output_buffer.io.read_req := true.B
        qrd_output_buffer.io.read_column := io.read_column
        qrd_output_buffer.io.read_row := io.read_row
        read_output_valid_reg := true.B
    }.otherwise {
        read_output_valid_reg := false.B
    }
    io.read_output.valid := read_output_valid_reg
    io.read_output.bits := Mux(read_output_valid_reg, qrd_output_buffer.io.read_output.bits, VecInit(Seq.fill(NUM_DOT_PE)(0.U.asTypeOf(new FloatType(8, 23)))))

    /*****************************
     * Logic for controlling PUs *
     ****************************/
    val done_reg = RegInit(VecInit(Seq.fill(PU_NUM)(false.B))) // Register that stores PU status

    val state_output_column = RegInit(0.U(log2Ceil(KEY_LENGTH).W))  // Column number that should be read from PUs
    val state_output_row = RegInit(0.U(log2Up(KEY_LENGTH / NUM_DOT_PE).W)) // Row number that should be read from PUs
    val state_output_column_reg = RegNext(state_output_column)
    val state_output_row_reg = RegNext(state_output_row)
    val state_output_read_reg = RegInit(VecInit(Seq.fill(PU_NUM)(false.B)))

    val state_input_column = RegInit(0.U(log2Ceil(KEY_LENGTH).W)) // Column number that should be written to PUs
    val state_input_row = RegInit(0.U(log2Up(KEY_LENGTH / NUM_DOT_PE).W)) // Row number that should be written to PUs
    val state_input_column_reg = RegNext(state_input_column)
    val state_input_row_reg = RegNext(state_input_row)
    val state_input_read_reg = RegInit(false.B)

    val is_second_reg = RegInit(false.B) // Store whether it is second loop of op 3

    /** ** Workflow for controlling PUs **
     *
     *  - op is 1: Use matrix loaded in the PUs
     *      - Used for general QRD & QRD of concat-ed matrix
     *      - Can specify number of PUs (1 PU in the most cases)
     * 
     *      1. Trigger PUs (# of PUs are specified in used_pus)
     *      2. Read result R matrix in the PUs & Store them to the output_buffer
     * 
     *      state_IDLE: Waiting for the signal
     *      state_TRIGGER: Trigger PUs
     *      state_WAITING: Waiting for the output
     *      state_OUTPUT: Read result R matrix in the PUs & Store them to the output_buffer
     *      state_DONE: Emit done signal
     * 
     *  - op is 2: Load matrix from the result buffer to the PUs
     *      - Used for concatenating result R matrix with memoized one
     *      - Always load entire result matrix to the first PU
     * 
     *      1. Read result R matrix in the output_buffer & Store them to the PUs
     * 
     *      state_IDLE: Waiting of the signal
     *      state_INPUT: Read result R matrix in the output_buffer & Store them to the PUs
     *      state_DONE: Emit done signal
     *  
     *  - op is 3: Do parallel QRD
     *      - Used for new key matrix
     *      - Can specify number of PUs for the first loop
     *      - Always use one PU (the first one) for the second loop
     * 
     *      1. Trigger PUs (# of PUs are specified in used_pus)
     *      2. Read result R matrix in the PUs & Store them to the output_buffer
     *      3. Read result R matrix in the output_buffer & Store them to the PUs (use only one PU)
     *      4. Read result R matrix in the PUs & Store them to the output_buffer (use only one PU)
     *      5. Operation done
     * 
     *      state_IDLE: Waiting for the signal
     *      state_TRIGGER: Trigger PUs
     *      state_WAITING: Waiting for the output
     *      state_OUTPUT: Read result R matrix in the PUs & Store them to the output_buffer
     *      state_INPUT: Read result R matrix in the output_buffer & Store them to the PUs
     *      state_TRIGGER: Trigger PUs
     *      state_WAITING: Waiting for the output
     *      state_OUTPUT: Read result R matrix in the PUs & Store them to the output_buffer
     *      state_DONE: Emit done signal
     */

    switch (state_reg) {
        is (state_IDLE) {
            when (io.op.valid === true.B) {
                current_op := io.op.bits
                switch (io.op.bits) {
                    is (0.U) { state_reg := state_IDLE }
                    is (1.U) { state_reg := state_TRIGGER }
                    is (2.U) { state_reg := state_INPUT }
                    is (3.U) { state_reg := state_TRIGGER }
                }
            }
        }
        is (state_INPUT) { // `current_pus` is used for inferring matrix size
            qrd_output_buffer.io.read_req := true.B
            qrd_output_buffer.io.read_column := state_input_column
            qrd_output_buffer.io.read_row := state_input_row
            state_input_read_reg := true.B

            when (state_input_row === (KEY_LENGTH / NUM_DOT_PE).U * current_pus - 1.U) {
                state_input_row := 0.U
                when (state_input_column === KEY_LENGTH.U - 1.U) {
                    switch (current_op) {
                        is (0.U) { state_reg := state_DONE }
                        is (1.U) { state_reg := state_DONE }
                        is (2.U) { state_reg := state_DONE}
                        is (3.U) { 
                            current_pus := 1.U
                            state_reg := state_TRIGGER
                        }
                    }
                } .otherwise {
                    state_input_column := state_input_column + 1.U
                }
            } .otherwise {
                state_input_row := state_input_row + 1.U
            }
        }
        is (state_TRIGGER) {
            assert(current_pus =/= 0.U)
            for (i <- 0 until PU_NUM) {
                pu_vec(i).io.op := i.U < current_pus
                done_reg(i) := false.B
            }
            state_reg := state_WAITING
        }
        is (state_WAITING) {
            val check_done = WireDefault(VecInit(Seq.fill(PU_NUM)(true.B)))
            for (i <- 0 until PU_NUM) {
                when(i.U < current_pus) {
                    done_reg(i) := Mux(pu_vec(i).io.done, true.B, done_reg(i))
                    check_done(i) := Mux(pu_vec(i).io.done, true.B, done_reg(i))
                }
            }

            when (check_done.reduce(_ & _)) {
                state_output_column := 0.U
                state_output_row := 0.U
                for (i <- 0 until PU_NUM) { state_output_read_reg(i) := false.B }
                state_reg := state_OUTPUT
            }
        }
        is (state_OUTPUT) {
            for (i <- 0 until PU_NUM) {
                when(i.U < current_pus) {
                    pu_vec(i).io.read_req := true.B
                    pu_vec(i).io.read_row := state_output_row
                    pu_vec(i).io.read_column := state_output_column
                    state_output_read_reg(i) := true.B
                } .otherwise {
                    state_output_read_reg(i) := false.B
                }
            }

            when (state_output_row === (KEY_LENGTH / NUM_DOT_PE).U - 1.U) {
                state_output_row := 0.U
                when (state_output_column === KEY_LENGTH.U - 1.U) {
                    //for (i <- 0 until PU_NUM) { state_output_read_reg(i) := false.B }
                    switch (current_op) {
                        is(0.U) { state_reg := state_DONE }
                        is(1.U) { state_reg := state_DONE }
                        is(2.U) { state_reg := state_DONE }
                        is(3.U) {
                            when(is_second_reg) {   // Second loop is done
                                state_reg := state_DONE
                            } .otherwise {          // First loop is done & Go to the second loop
                                is_second_reg := true.B
                                //current_pus := 1.U  // Second loop always use one PU
                                state_reg := state_INPUT
                            }
                        }
                    }
                } .otherwise {
                    state_output_column := state_output_column + 1.U
                }
            } .otherwise {
                state_output_row := state_output_row + 1.U
            }
        }
        is (state_DONE) {
            current_op := 0.U
            current_pus := 0.U

            state_output_column := 0.U
            state_output_row := 0.U
            state_input_column := 0.U
            state_input_row := 0.U

            state_input_read_reg := false.B
            for (i <- 0 until PU_NUM) { state_output_read_reg(i) := false.B }
            is_second_reg := false.B

            state_reg := state_IDLE
            io.done := true.B
        }
    }

    /*********************************
     * Logic for PU -> output buffer *
     *********************************/
    for (i <- 0 until PU_NUM) {
        when(state_output_read_reg(i)) {
            assert(state_output_read_reg(i) === pu_vec(i).io.output.valid)
            qrd_output_buffer.io.write_req(i) := state_output_read_reg(i)
            qrd_output_buffer.io.write_input(i) := pu_vec(i).io.output.bits
            qrd_output_buffer.io.write_column(i) := state_output_column_reg
            qrd_output_buffer.io.write_row(i) := state_output_row_reg
        }
    }

    /************************************
     * Logic for output buffer -> PU(0) *
     ************************************/
    when ((state_reg =/= state_IDLE) && state_input_read_reg) {
        assert(state_input_read_reg === qrd_output_buffer.io.read_output.valid)
        pu_vec(0).io.write_req := true.B
        pu_vec(0).io.write_column := state_input_column_reg
        pu_vec(0).io.write_row := state_input_row_reg
        pu_vec(0).io.input := qrd_output_buffer.io.read_output.bits
    }
}

class QRD_Unit_Wrapper(
    TE_NUM: Int,
    PU_NUM: Int,
    PE_NUM: Int,
    KEY_LENGTH: Int,
    NUM_DOT_PE: Int,
    MEM_SIZE: Int,
    QUEUE_SIZE: Int
) extends Module {
    val io = IO(new Bundle{
        val write_req = Flipped(new Valid(UInt(log2Ceil(PU_NUM).max(1).W)))
        val write_column = Input(UInt(log2Ceil(KEY_LENGTH).W))
        val write_row = Input(UInt(log2Ceil(MEM_SIZE).W))
        val write_input = Input(new FloatType(8, 23))

        val read_req = Input(Bool())
        val read_column = Input(UInt(log2Ceil(KEY_LENGTH).W))
        val read_row = Input(UInt(log2Ceil(KEY_LENGTH * PU_NUM).W))
        val read_output = new Valid(Vec(NUM_DOT_PE, new FloatType(8, 23)))

        val op = Flipped(new Valid(UInt(2.W)))
        val used_pus = Flipped(new Valid(UInt(log2Ceil(PU_NUM).max(1).W)))
        val lengths = Flipped(new Valid(UInt(log2Ceil(MEM_SIZE).W)))
        val done = Output(Bool())
    })

    val qrd_units = Seq.fill(TE_NUM)(
        Module(new QRD_Unit(PU_NUM, PE_NUM, KEY_LENGTH, NUM_DOT_PE, MEM_SIZE, QUEUE_SIZE))
    )

    val read_output_valid = WireDefault(VecInit(Seq.fill(TE_NUM)(false.B)))
    
    val dones = WireDefault(VecInit(Seq.fill(TE_NUM)(false.B)))
    for (i <- 0 until TE_NUM) {
        qrd_units(i).io.write_req := io.write_req
        qrd_units(i).io.write_column := io.write_column + i.U
        qrd_units(i).io.write_row := io.write_row + i.U
        qrd_units(i).io.write_input := VecInit(Seq.fill(NUM_DOT_PE)(io.write_input))
        qrd_units(i).io.read_req := io.read_req
        qrd_units(i).io.read_column := io.read_column + i.U
        qrd_units(i).io.read_row := io.read_row + i.U
        qrd_units(i).io.op := io.op
        qrd_units(i).io.used_pus := io.used_pus
        qrd_units(i).io.lengths.valid := io.lengths.valid
        qrd_units(i).io.lengths.bits := VecInit(Seq.fill(PU_NUM)(io.lengths.bits))

        read_output_valid(i) := qrd_units(i).io.read_output.valid
        dones(i) := qrd_units(i).io.done
    }
    
    io.read_output.valid := read_output_valid.reduce(_ & _)
    for (i <- 0 until NUM_DOT_PE) {
        val read_output_bits = WireDefault(VecInit(Seq.fill(TE_NUM)(0.U(32.W))))
        for (j <- 0 until TE_NUM) {
            read_output_bits(j) := qrd_units(j).io.read_output.bits(i).asTypeOf(UInt(32.W))
        }
        io.read_output.bits(i) := read_output_bits.reduce(_ & _).asTypeOf(new FloatType(8, 23))
    }
    io.done := dones.reduce(_ & _)
}

object QRD_Unit extends App {
    // TE_NUM, PU_NUM, PE_NUM, KEY_LENGTH, NUM_DOT_PE, MEM_SIZE, QUEUE_SIZE
    /*val config_list: List[(Int, Int, Int)] = List(
        (3, 1, 8),
        (3, 8, 1),
        (2, 3, 4),
        (2, 4, 3),
        (1, 12, 2),
        (1, 6, 4),
        (1, 4, 6),
        (1, 2, 12),
        (4, 1, 6),
        (4, 2, 3),
        (4, 3, 2),
        (4, 6, 1),
    )
    for (config <- config_list) {
        val (numTE, numPU, numPE) = config
        println(s"Compilling ${numTE}_${numPU}_${numPE}")
        (new chisel3.stage.ChiselStage).emitVerilog(
            new QRD_Unit_Wrapper(numTE, numPU, numPE, 32, 8, 64, 64),
            Array("--target-dir", s"generated/${numTE}_${numPU}_${numPE}")
        )
    }*/
    val config_list: List[(Int, Int)] = List(
        (1, 8),
        (8, 1),
        (4, 2),
        (3, 4),
        (4, 3),
        (12, 2),
        (6, 4),
        (4, 6),
        (2, 12),
        (2, 3),
        (3, 2),
    )
    for (config <- config_list) {
        val (numPU, numPE) = config
        println(s"Compilling ${numPU}_${numPE}")
        (new chisel3.stage.ChiselStage).emitVerilog(
            new QRD_Unit(numPU, numPE, 32, 8, 64, 64),
            Array("--target-dir", s"generated/${numPU}_${numPE}")
        )
    }
}

/**
  * QRD_Output_Buffer: Buffer for storing result R matrix
  *     - Write address is per-PU address
  *     - Read address is global address
  * 
  * @param PU_NUM: Total number of PUs
  * @param KEY_LENGTH: Key length of learned index (Row/Column size of R matrix)
  * @param NUM_DOT_PE: Number of parallel multipliers at dot product tree unit
  */
class QRD_Output_Buffer(PU_NUM: Int, KEY_LENGTH: Int, NUM_DOT_PE: Int) extends Module {
    val io = IO(new Bundle {
        val write_req = Input(Vec(PU_NUM, Bool()))
        val write_column = Input(Vec(PU_NUM, UInt(log2Ceil(KEY_LENGTH).W)))
        val write_row = Input(Vec(PU_NUM, UInt(log2Ceil(KEY_LENGTH).W)))
        val write_input = Input(Vec(PU_NUM, Vec(NUM_DOT_PE, new FloatType(8, 23))))

        val read_req = Input(Bool())
        val read_column = Input(UInt(log2Ceil(KEY_LENGTH).W))
        val read_row = Input(UInt(log2Ceil(KEY_LENGTH * PU_NUM).W))
        val read_output = new Valid(Vec(NUM_DOT_PE, new FloatType(8, 23)))
    })
    assert(KEY_LENGTH % NUM_DOT_PE == 0) // TODO: Support the case KEY_LEGNTH % NUM_DOT_PE != 0

    val mem = Seq.fill(PU_NUM)(
        Module(new Matrix_Buffer_Wrapper(KEY_LENGTH, 1, NUM_DOT_PE, KEY_LENGTH / NUM_DOT_PE))
    )

    // Write logic
    for (i <- 0 until PU_NUM) {
        mem(i).io.write_req(0) := io.write_req(i)
        mem(i).io.write_column(0) := io.write_column(i)
        mem(i).io.write_row(0) := io.write_row(i)
        mem(i).io.input(0) := io.write_input(i)
    }

    // Convert global address to per-pu address
    val read_pu = io.read_row >> log2Up(KEY_LENGTH / NUM_DOT_PE).U
    val read_row_pu = io.read_row(log2Up(KEY_LENGTH / NUM_DOT_PE) - 1, 0)
    val read_pu_reg = RegNext(read_pu)

    // Read logic
    io.read_output.bits := VecInit(Seq.fill(NUM_DOT_PE)(0.U.asTypeOf(new FloatType(8, 23)))) 
    for (i <- 0 until PU_NUM) {
        when(i.U === read_pu) {
            mem(i).io.read_req(0) := true.B
            mem(i).io.read_column(0) := io.read_column
            mem(i).io.read_row(0) := read_row_pu
        } .otherwise {
            mem(i).io.read_req(0) := false.B
            mem(i).io.read_column(0) := 0.U
            mem(i).io.read_row(0) := 0.U
        }

        when(i.U === read_pu_reg) {
            io.read_output.bits := mem(i).io.output(0)
        }
    }

    val valid_reg = RegNext(io.read_req)
    io.read_output.valid := valid_reg
}