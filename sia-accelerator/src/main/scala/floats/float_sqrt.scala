package floats

import chisel3._
import chisel3.util._
import scala.io.Source
import scala.math._

/*
 * FloatSqrt: floating point sqrt
 *      - Reads Lookup table and returns approximated value
 *      cycles: 3
 */

class FloatSqrt(index_bits: Int) extends Module {
    val io = IO(new Bundle {
        val input = Input(new FloatType(8, 23))
        val output = Output(new FloatType(8, 23))
    })

    // Build LUT table
    val sqrt_table = new Array[Int](1 << index_bits)
    val sqrt_table_source = Source.fromFile("./src/main/scala/floats/sqrt_table/sqrt_table.txt")

    var idx = 0
    for (line <- sqrt_table_source.getLines()) {
        sqrt_table(idx) = line.toInt
        idx += 1
    }

    val ROM = VecInit(sqrt_table.map(_.U(23.W)))

    // Get sqrt frac from first 8 bits of input frac
    val sqrt_frac = ROM(io.input.frac(23-1, 23-index_bits) + io.input.frac(23-1-index_bits)) // Do Rounding

    // Multiply sqrt(2) if needed
    val multiplier = Module(new FloatMult)

    multiplier.io.input_a.sign := io.input.sign
    multiplier.io.input_a.exp := Mux(io.input.exp === 0.U, 0.U,((io.input.exp + 1.U) >> 1.U) + 63.U)
    multiplier.io.input_a.frac := sqrt_frac

    multiplier.io.input_b.sign := 0.U(1.W)
    multiplier.io.input_b.exp := 127.U(8.W)

    when (io.input.exp(0) === 0.U) {
        // Should multiply sqrt(2)
        multiplier.io.input_b.frac := 3474675.U(23.W)
    } .otherwise {
        // Should multiply sqrt(1)
        multiplier.io.input_b.frac := 0.U(23.W)
    }

    io.output := multiplier.io.output    
}

object FloatSqrt extends App {
    (new chisel3.stage.ChiselStage).emitVerilog(new FloatSqrt(8), Array("--target-dir", "generated/floats", "--no-constant-propagation"))
}