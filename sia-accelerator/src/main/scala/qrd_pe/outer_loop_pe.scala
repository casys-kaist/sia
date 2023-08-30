package qrd_pe

import chisel3._
import chisel3.util._
import floats.FloatType
import os.write

/**
 *  OuterLoopPE: Calculate householder reflector for the given vector
 *      - start : valid indicates module start signal
 *              : bits indicate vector length (x8)
 *      - input_vec : valid indicates valid input data
 *                  : bits indicate a vector of 8 input data (zero padded) -- DELAYED WITH 4 CYCLES
 *      - output_reflector : valid indicates valid output data
 *                         : bits indicate 8 output data (zero padded)
 *      - output_gamma : valid indicates valid output data
 *                     : bits indicate gamma output data
 *      - done : indicates module operation done
 * 
 * @param NUM_DOT_PE: Number of parallel multipliers at dot product tree unit
 * @param QUEUE_SIZE: Maximum depth of the queue
 */
class OuterLoopPE(NUM_DOT_PE: Int, QUEUE_SIZE: Int) extends Module {
    val io = IO(new Bundle{
        val start = Flipped(new Valid(UInt(8.W)))
        val input = Flipped(new Valid(Vec(NUM_DOT_PE, new FloatType(8, 23))))

        val output_reflector = new Valid(Vec(NUM_DOT_PE, new FloatType(8, 23)))
        val output_gamma = new Valid(new FloatType(8, 23))
        val done = Output(UInt(1.W))
    })

    val domain1_module = Module(new OuterLoopPE_domain1(NUM_DOT_PE))
    val domain2_module = Module(new OuterLoopPE_domain2(NUM_DOT_PE, QUEUE_SIZE))

    /*********************
     *  Datapath Logics  *
     *********************/
    domain1_module.io.input := io.input
    domain2_module.io.input := io.input

    domain1_module.io.start := io.start
    domain2_module.io.u_k_input := domain1_module.io.u_k_output

    io.output_reflector := domain2_module.io.output
    io.output_gamma := domain1_module.io.gamma_output

    /***********************
     *  Controller Logics  *
     ***********************/
    val reflector_done_reg = RegInit(0.U(1.W))
    val reflector_done_wire = WireDefault(0.U(1.W))
    val gamma_done_reg = RegInit(0.U(1.W))
    val gamma_done_wire = WireDefault(0.U(1.W))
    val length_reg = RegInit(0.U(8.W))

    when (io.start.valid === true.B) {
        length_reg := io.start.bits
    } .elsewhen(io.output_reflector.valid === 1.U) {
        length_reg := length_reg - 1.U
    }

    reflector_done_wire := reflector_done_reg
    gamma_done_wire := gamma_done_reg

    when ((length_reg === 1.U) && (io.output_reflector.valid === 1.U)) {
        reflector_done_reg := 1.U
        reflector_done_wire := 1.U
    }

    when (domain1_module.io.gamma_output.valid === 1.U) {
        gamma_done_reg := 1.U
        gamma_done_wire := 1.U
    }

    io.done := reflector_done_wire & gamma_done_wire
}

/**
  * OuterLoopPE_domain1: receives input vector (8 elements per cycle)
  *                      calculates u_k(1), r_k and returns them
  *
  * @param NUM_DOT_PE: Number of parallel multipliers at dot product tree unit
  */
class OuterLoopPE_domain1(NUM_DOT_PE: Int) extends Module {
    val io = IO(new Bundle{
        val start = Flipped(new Valid(UInt(8.W)))
        val input = Flipped(new Valid(Vec(NUM_DOT_PE, (new FloatType(8, 23)))))
        val gamma_output = new Valid(new FloatType(8, 23))
        val u_k_output = new Valid(new FloatType(8, 23))
    })

    // Module Declarations
    val dot_product_unit = Module(new DotProductTree(NUM_DOT_PE))
    val get_first_reg = Module(new GetFirstReg())               // Register that memorize the first element
    val dp_pipe = Module(new Pipe(new FloatType(8, 23), 6))     // Pipeline for the dot product result

    val sqrt = Module(new floats.FloatSqrtValid())
    val multiplier = Module(new floats.FloatMultValid())
    val u_k_adder = Module(new floats.FloatAddValid())
    val gamma_adder = Module(new floats.FloatAddValid())
    val divider = Module(new floats.FloatOneDivValid())

    // Inputs
    dot_product_unit.io.start := io.start
    dot_product_unit.io.input_a := io.input
    dot_product_unit.io.input_b := io.input
    get_first_reg.io.input.bits := io.input.bits(0)
    get_first_reg.io.input.valid := io.input.valid

    // Common Path
    dp_pipe.io.enq := dot_product_unit.io.output
    sqrt.io.input := dot_product_unit.io.output

    val sign_multiplier = Wire(new Valid(new FloatType(8, 23)))
    sign_multiplier := sqrt.io.output
    sign_multiplier.bits.sign := get_first_reg.io.output.bits.sign ^ sqrt.io.output.bits.sign

    // u_k(1) Outputs
    u_k_adder.io.input_a := sign_multiplier
    u_k_adder.io.input_b := get_first_reg.io.output
    io.u_k_output := u_k_adder.io.output

    // gamma Outputs
    multiplier.io.input_a := sign_multiplier
    multiplier.io.input_b := get_first_reg.io.output
    gamma_adder.io.input_a := multiplier.io.output
    gamma_adder.io.input_b := dp_pipe.io.deq
    divider.io.input := gamma_adder.io.output
    io.gamma_output := divider.io.output
}

/**
  * OuterLoopPE_domain2: receives input vector (with streaming manner) and u_k(1)
  *                      apply u_k(1) to the input vector
  * 
  *
  * @param NUM_DOT_PE: Number of parallel multipliers at dot product tree unit
  * @param QUEUE_SIZE: Maximum depth of the queue
  * 
  */
class OuterLoopPE_domain2(NUM_DOT_PE: Int, QUEUE_SIZE: Int) extends Module {
    val io = IO(new Bundle {
        val input = Flipped(new Valid(Vec(NUM_DOT_PE, (new FloatType(8, 23)))))
        val u_k_input = Flipped(new Valid(new FloatType(8, 23)))
        val output = new Valid(Vec(NUM_DOT_PE, new FloatType(8, 23)))
    })

    // Input queue
    val input_queue = Module(new Queue(Vec(NUM_DOT_PE, UInt(32.W))
                                        , QUEUE_SIZE, false, true, true, false))
    input_queue.io.enq.bits := VecInit(io.input.bits.map(_.asTypeOf(UInt(32.W))))
    input_queue.io.enq.valid := io.input.valid
    assert(input_queue.io.enq.ready === true.B) // Queue must not be full

    // State definition & modules
    val state_wait_uk :: state_processing_first :: state_processing_remain :: Nil = Enum(3)
    val state_reg = RegInit(state_wait_uk)
    val u_k_reg = RegInit((0.U).asTypeOf(new FloatType(8, 23)))

    // Default values
    input_queue.io.deq.ready := true.B
    io.output.valid := false.B
    for (i <- 0 until NUM_DOT_PE) {
        io.output.bits(i) := (0.U).asTypeOf(new FloatType(8, 23))
    }

    // State machine
    switch (state_reg) {
        is (state_wait_uk) {
            when(io.u_k_input.valid === true.B) {
                u_k_reg := io.u_k_input.bits
                state_reg := state_processing_first
                input_queue.io.deq.ready := false.B
            } .otherwise { 
                input_queue.io.deq.ready := false.B
            }
        }
        is (state_processing_first) {
            when(input_queue.io.deq.valid === true.B) {
                io.output.valid := true.B
                io.output.bits(0) := u_k_reg
                for (i <- 1 until NUM_DOT_PE) {
                    io.output.bits(i) := input_queue.io.deq.bits(i).asTypeOf(new FloatType(8, 23))
                }
                state_reg := state_processing_remain
            }
        }
        is (state_processing_remain) {
            when(input_queue.io.deq.valid === true.B) {
                io.output.valid := true.B
                for (i <- 0 until NUM_DOT_PE) {
                    io.output.bits(i) := input_queue.io.deq.bits(i).asTypeOf(new FloatType(8, 23))
                }
            }
        }
    }
}

object OuterLoopPE extends App {
    (new chisel3.stage.ChiselStage).emitVerilog(new OuterLoopPE(8, 256), Array("--target-dir", "generated/qrd_pe"))
}