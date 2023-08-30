package qrd_pe

import chisel3._
import chisel3.util._
import floats.FloatType

/*
 * DotProductTree: Fully pipelined dot product unit
 *      ! Inputs must be delayed with 4 cycles  !
 *      ! I X X X X I X X X X I X X ...         !
 * 
 *      1. Parallel Multiplier: 3 cycles
 *      2. Adder Tree: 3 cycles * (3 if NUM_DOT_PE is 8, 4 if NUM_DOT_PE is 16)
 *      3. Accumulator: 3 cycles
 */

class DotProductTree(NUM_DOT_PE: Int) extends Module {
    val io = IO(new Bundle{
        val start = Flipped(new Valid(UInt(8.W)))
        val input_a = Flipped(new Valid(Vec(NUM_DOT_PE, (new FloatType(8, 23)))))
        val input_b = Flipped(new Valid(Vec(NUM_DOT_PE, (new FloatType(8, 23)))))

        val output = new Valid(new FloatType(8, 23))
    })

    assert(util.isPow2(NUM_DOT_PE), "DotProductTree: NUM_DOT_PE must be power of two!")

    /*********************
     *  Datapath Logics  *
     *********************/
    // Stage 1: Parallel Multiplier
    val multiplier_results = Wire(Vec(NUM_DOT_PE, new FloatType(8, 23)))
    val multipliers = Array.fill(NUM_DOT_PE) {
        Module(new floats.FloatMult)
    }
    val valid_pipeline = Module(new Pipe(Bool(), 3))
    valid_pipeline.io.enq.valid := 1.U
    valid_pipeline.io.enq.bits := (io.input_a.valid & io.input_b.valid)

    for (i <- 0 until NUM_DOT_PE) {
        multipliers(i).io.input_a := io.input_a.bits(i)
        multipliers(i).io.input_b := io.input_b.bits(i)
        multiplier_results(i) := multipliers(i).io.output
    }

    // Stage 2: Adder Tree
    var current_level: Int = NUM_DOT_PE
    var adder_tree_levels = Array(Module(new AdderTreeLevel(current_level)))
    current_level = current_level / 2
    while (current_level > 1) {
        adder_tree_levels :+= Module(new AdderTreeLevel(current_level))
        current_level = current_level / 2
    }

    adder_tree_levels(0).io.input_vec.bits := multiplier_results
    adder_tree_levels(0).io.input_vec.valid := valid_pipeline.io.deq.bits
    for (i <- 1 until adder_tree_levels.length) {
        adder_tree_levels(i).io.input_vec := adder_tree_levels(i-1).io.output_vec
    }
    val adder_tree_result = Wire(new Valid(new FloatType(8, 23)))
    adder_tree_result.bits := adder_tree_levels(adder_tree_levels.length - 1).io.output_vec.bits(0)
    adder_tree_result.valid := adder_tree_levels(adder_tree_levels.length - 1).io.output_vec.valid

    // Stage 3: Accumulator
    val accumulator_init = Wire(new Valid(new FloatType(8, 23)))
    accumulator_init.valid := true.B
    accumulator_init.bits := 0.U.asTypeOf(new FloatType(8, 23))

    val accumulator = RegInit(accumulator_init)
    val accumulator_adder = Module(new floats.FloatAddValid)
    accumulator_adder.io.input_a := accumulator
    accumulator_adder.io.input_b := adder_tree_result
    accumulator.bits := accumulator_adder.io.output.bits
    accumulator.valid := 1.U

    /*********************
     * Controller Logics *
     *********************/
    // When module start, update length register
    val length_reg = RegInit(0.U(8.W))
    when (io.start.valid === 1.U) {
        length_reg := io.start.bits
    } .elsewhen(accumulator_adder.io.output.valid === 1.U) {
        length_reg := length_reg - 1.U
    } .otherwise {
        length_reg := length_reg
    }

    when ((length_reg === 1.U) && (accumulator_adder.io.output.valid === 1.U)) {
        io.output.valid := 1.U
    } .otherwise {
        io.output.valid := 0.U
    }
    io.output.bits := accumulator_adder.io.output.bits
}

class AdderTreeLevel(input_vector_length: Int) extends Module {
    val io = IO(new Bundle{
        val input_vec = Flipped(new Valid(Vec(input_vector_length, new FloatType(8, 23))))
        val output_vec = new Valid(Vec(input_vector_length / 2, new FloatType(8, 23)))
    })

    assert(input_vector_length % 2 == 0, "AdderTreeLevel: input_vector_length must be power of two!")

    val valid_pipeline = Module(new Pipe(Bool(), 4))
    valid_pipeline.io.enq.valid := 1.U
    valid_pipeline.io.enq.bits := io.input_vec.valid

    val adders = Array.fill(input_vector_length / 2) {
        Module(new floats.FloatAdd)
    }

    for (i <- 0 until input_vector_length / 2) {
        adders(i).io.input_a := io.input_vec.bits(i * 2)
        adders(i).io.input_b := io.input_vec.bits(i * 2 + 1)
        io.output_vec.bits(i) := adders(i).io.output
    }
    io.output_vec.valid := valid_pipeline.io.deq.bits
}

object DotProductTree extends App {
    (new chisel3.stage.ChiselStage).emitVerilog(new DotProductTree(8), Array("--target-dir", "generated/qrd_pe"))
}