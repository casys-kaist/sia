package qrd_pu

import chisel3._
import chisel3.util._
import floats.FloatType

/**
 * Scalar_Register: Store gamma scalar value and **always** returns it
 *      - input: valid indicates valid input data
 *             : bits indicate input gamma value
 *      - reset: reset
 *      - output: valid indicates valid output data
 *              : bits indicate output gamma value
 */
class Scalar_Register extends Module{
    val io = IO(new Bundle {
        val input = Flipped(new Valid(new FloatType(8, 23)))
        val reset = Input(Bool())
        val output = new Valid(new FloatType(8, 23))
    })

    val gamma_reg = RegInit((0.U).asTypeOf(new FloatType(8, 23)))
    val gamma_valid_reg = RegInit(false.B)

    when (io.reset === false.B) {
        when ((gamma_valid_reg === false.B) & (io.input.valid === true.B)) {
            gamma_reg := io.input.bits
            gamma_valid_reg := io.input.valid
        }
    } .otherwise {
        gamma_reg := (0.U).asTypeOf(new FloatType(8, 23))
        gamma_valid_reg := false.B
    }

    io.output.valid := gamma_valid_reg
    io.output.bits := gamma_reg
}
