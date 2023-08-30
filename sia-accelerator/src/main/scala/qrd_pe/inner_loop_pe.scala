package qrd_pe

import chisel3._
import chisel3.util._
import floats.FloatType
import floats.FloatMultValid

/**
  * InnerLoopPE: Apply householder reflector to input a vector
  *     - start : valid indicates module start signal
  *             : bits indicate vector length (x8)
  *     - input_u : valid indicates valid input data
  *               : bits indicate a vector of 8 input data (zero padded) -- DELAYED WITH 4 CYCLES
  *               : A reflector vector calculated at the outer loop PE
  *     - input_a : valid indicates valid input data
  *               : bits indicate a vector of 8 input data (zero padded) -- DELAYED WITH 4 CYCLES
  *               : An input vector reflector vector is applied to
  *     - input_gamma : valid indicates valid input data
  *                   : bits indicate gamma input data calculated at the outer loop PE
  *     - output : valid indicates valid output data
  *              : bits indicate a vector of 8 output data (zero padded)
  *     - done : indicates module operation done
  *
  * @param NUM_DOT_PE: Number of parallel multipliers at dot product tree unit
  * @param QUEUE_SIZE: Maximum depth of the queue
  */
class InnerLoopPE(NUM_DOT_PE: Int, QUEUE_SIZE: Int) extends Module {
    val io = IO(new Bundle{
        val start = Flipped(new Valid(UInt(8.W)))
        val input_u = Flipped(new Valid(Vec(NUM_DOT_PE, new FloatType(8, 23))))
        val input_a = Flipped(new Valid(Vec(NUM_DOT_PE, new FloatType(8, 23))))
        val input_gamma = Flipped(new Valid(new FloatType(8, 23)))
        val reset = Input(Bool())

        val output = new Valid(Vec(NUM_DOT_PE, new FloatType(8, 23)))
        val done = Output(UInt(1.W))
    })
    val domain1_module = withReset(io.reset) { Module(new InnerLoopPE_domain1(NUM_DOT_PE)) }
    val domain2_module = withReset(io.reset) { Module(new InnerLoopPE_domain2(NUM_DOT_PE, QUEUE_SIZE)) }

    /***********************
     *   Datapath Logics   *
     ***********************/
    domain1_module.io.start := io.start
    domain1_module.io.input_a := io.input_a
    domain1_module.io.input_u := io.input_u
    domain1_module.io.input_gamma := io.input_gamma

    domain2_module.io.input_a := io.input_a
    domain2_module.io.input_u := io.input_u
    domain2_module.io.input_coeff := domain1_module.io.output

    io.output := domain2_module.io.output

    /***********************
     *  Controller Logics  *
     ***********************/
    val length_reg = withReset(io.reset) { RegInit(0.U(8.W)) }
    val vector_done_reg = withReset(io.reset) { RegInit(0.U(1.W)) }
    val vector_done_wire = withReset(io.reset) { WireDefault(0.U(1.W)) }

    when (io.start.valid === true.B) {
        length_reg := io.start.bits
    } .elsewhen(domain2_module.io.output.valid === 1.U) {
        length_reg := length_reg - 1.U
    }

    vector_done_wire := vector_done_reg

    when ((length_reg === 1.U) && (domain2_module.io.output.valid === 1.U)) {
        vector_done_reg := 1.U
        vector_done_wire := 1.U
    }

    io.done := vector_done_wire
}

/**
  * InnerLoopPE_domain1: calculate dot product and multiply with gamma value
  *
  * @param NUM_DOT_PE
  */
class InnerLoopPE_domain1(NUM_DOT_PE: Int) extends Module {
    val io = IO(new Bundle{
        val start = Flipped(new Valid(UInt(8.W)))
        val input_u = Flipped(new Valid(Vec(NUM_DOT_PE, (new FloatType(8, 23)))))
        val input_a = Flipped(new Valid(Vec(NUM_DOT_PE, (new FloatType(8, 23)))))
        val input_gamma = Flipped(new Valid(new FloatType(8, 23)))

        val output = new Valid(new FloatType(8, 23))
    })
    // Module Declarations
    val dot_product_unit = Module(new DotProductTree(NUM_DOT_PE))

    val dp_reginit = Wire(new Valid(new FloatType(8, 23)))
    dp_reginit.valid := false.B
    dp_reginit.bits := 0.U.asTypeOf(new FloatType(8, 23))
    val dp_reg = RegInit(dp_reginit)

    val gamma_reginit = Wire(new Valid(new FloatType(8, 23)))
    gamma_reginit.valid := false.B
    gamma_reginit.bits := 0.U.asTypeOf(new FloatType(8, 23))
    val gamma_reg = RegInit(gamma_reginit)
    
    val multiplier = Module(new FloatMultValid())
    multiplier.reset := reset

    // Module Connections
    dot_product_unit.io.start := io.start
    dot_product_unit.io.input_a := io.input_a
    dot_product_unit.io.input_b := io.input_u

    when((gamma_reg.valid === false.B) & (io.input_gamma.valid === true.B)) {
        gamma_reg := io.input_gamma
    }

    when((dp_reg.valid === false.B) & (dot_product_unit.io.output.valid === true.B)) {
        dp_reg := dot_product_unit.io.output
    }

    multiplier.io.input_a := gamma_reg
    multiplier.io.input_b := dp_reg
    io.output := multiplier.io.output
}

/**
  * InnerLoopPE_domain2: apply reflector to input a vector
  *
  * @param NUM_DOT_PE
  * @param QUEUE_SIZE
  */
class InnerLoopPE_domain2(NUM_DOT_PE: Int, QUEUE_SIZE: Int) extends Module {
    val io = IO(new Bundle {
        val input_u = Flipped(new Valid(Vec(NUM_DOT_PE, (new FloatType(8, 23)))))
        val input_a = Flipped(new Valid(Vec(NUM_DOT_PE, (new FloatType(8, 23)))))
        val input_coeff = Flipped(new Valid(new FloatType(8, 23)))

        val output = new Valid(Vec(NUM_DOT_PE, new FloatType(8, 23)))
    })
    // Input Queues
    val input_u_queue = Module(new Queue(Vec(NUM_DOT_PE, UInt(32.W)),
                                        QUEUE_SIZE, false, true, true, false))
    val input_a_queue = Module(new Queue(Vec(NUM_DOT_PE, UInt(32.W)),
                                        QUEUE_SIZE, false, true, true, false))

    input_u_queue.io.enq.bits := VecInit(io.input_u.bits.map(_.asTypeOf(UInt(32.W))))
    input_u_queue.io.enq.valid := io.input_u.valid
    input_a_queue.io.enq.bits := VecInit(io.input_a.bits.map(_.asTypeOf(UInt(32.W))))
    input_a_queue.io.enq.valid := io.input_a.valid
    assert(input_a_queue.io.enq.ready === true.B)
    assert(input_u_queue.io.enq.ready === true.B)

    // State Definition & Modules
    val d4_reg = withReset(reset) { RegInit((0.U).asTypeOf(new FloatType(8, 23))) }
    val d4_valid_reg = withReset(reset) { RegInit(false.B) }

    // Module Connection
    val multipliers = Array.fill(NUM_DOT_PE) {
        Module(new floats.FloatMultValid())
    }
    val adders = Array.fill(NUM_DOT_PE) {
        Module(new floats.FloatAddValid())
    }

    /*********************
     *  Datapath Logics  *
     *********************/
    for (i <- 0 until NUM_DOT_PE) {
        multipliers(i).io.input_a.bits := d4_reg
        multipliers(i).io.input_a.valid := d4_valid_reg
        multipliers(i).io.input_b.bits := input_u_queue.io.deq.bits(i).asTypeOf(new FloatType(8, 23))
        multipliers(i).io.input_b.valid := input_u_queue.io.deq.valid
        input_u_queue.io.deq.ready := d4_valid_reg

        val adder_input_a_reg = RegNext(multipliers(i).io.output)
        val adder_input_b_bits_reg = RegNext(input_a_queue.io.deq.bits(i).asTypeOf(new FloatType(8, 23)))
        val adder_input_b_valid_reg = RegNext(input_a_queue.io.deq.valid)
        adders(i).io.input_a := adder_input_a_reg
        adders(i).io.input_b.valid := adder_input_b_valid_reg
        adders(i).io.input_b.bits := adder_input_b_bits_reg
        input_a_queue.io.deq.ready := adder_input_a_reg.valid

        io.output.bits(i) := adders(i).io.output.bits
    }
    io.output.valid := adders(0).io.output.valid

    /*********************
     * Controller Logics *
     *********************/
    when((d4_valid_reg === false.B) & (io.input_coeff.valid === true.B)) {
        d4_reg := io.input_coeff.bits
        d4_valid_reg := true.B
    }

    when(d4_valid_reg === true.B) {
        input_u_queue.io.deq.ready := true.B
        when(multipliers(0).io.output.valid === true.B) {
            input_a_queue.io.deq.ready := true.B
        } .otherwise {
            input_a_queue.io.deq.ready := false.B
        }
    } .otherwise {
        input_a_queue.io.deq.ready := false.B
        input_u_queue.io.deq.ready := false.B
    }
}

object InnerLoopPE extends App {
    (new chisel3.stage.ChiselStage).emitVerilog(new InnerLoopPE(8, 256), Array("--target-dir", "generated/qrd_pe"))
}