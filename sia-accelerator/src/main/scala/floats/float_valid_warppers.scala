package floats

import chisel3._
import chisel3.util._

/*
 * FloatAddValid: Wrapper for FloatAdd module with valid signals
 *      cycles: 4
 */

class FloatAddValid extends Module {
    val io = IO(new Bundle{
        val input_a = Flipped(new Valid(new FloatType(8, 23)))
        val input_b = Flipped(new Valid(new FloatType(8, 23)))
        val output = new Valid(new FloatType(8, 23))
    })

    val adder = Module(new FloatAdd())
    val pipe = ShiftRegister(io.input_a.valid & io.input_b.valid, 4, false.B, true.B)

    adder.io.input_a := io.input_a.bits
    adder.io.input_b := io.input_b.bits

    io.output.valid := pipe
    io.output.bits := adder.io.output
}

/*
 * FloatMultValid: Wrapper for FloadMult module with valid signals
 *      cycles: 3
 */

class FloatMultValid extends Module {
    val io = IO(new Bundle{
        val input_a = Flipped(new Valid(new FloatType(8, 23)))
        val input_b = Flipped(new Valid(new FloatType(8, 23)))
        val output = new Valid(new FloatType(8, 23))
    })

    val pipe = ShiftRegister(io.input_a.valid & io.input_b.valid, 3, false.B, true.B)
    val multiplier = Module(new FloatMult())

    multiplier.io.input_a := io.input_a.bits
    multiplier.io.input_b := io.input_b.bits

    io.output.valid := pipe
    io.output.bits := multiplier.io.output
}

/*
 * FloatOneDivValid: Wrapper for FloadTwoDiv module with valid signals
 *      cycles: 26
 */

class FloatOneDivValid extends Module {
    val io = IO(new Bundle{
        val input = Flipped(new Valid(new FloatType(8, 23)))
        val output = new Valid(new FloatType(8, 23))
    })

    val divider = Module(new FloatOneDiv())
    val pipe = ShiftRegister(io.input.valid, 26, false.B, true.B)

    divider.io.input := io.input.bits

    io.output.valid := pipe
    io.output.bits := divider.io.output
}

/*
 * FloatSqrtValid: Wrapper for FloadSqrt module with valid signals
 *      cycles: 3
 */

 class FloatSqrtValid extends Module {
    val io = IO(new Bundle{
        val input = Flipped(new Valid(new FloatType(8, 23)))
        val output = new Valid(new FloatType(8, 23))
    })

    val sqrt = Module(new FloatSqrt(8))
    val pipe = ShiftRegister(io.input.valid, 3, false.B, true.B)

    sqrt.io.input := io.input.bits
    io.output.valid := pipe
    io.output.bits := sqrt.io.output
}