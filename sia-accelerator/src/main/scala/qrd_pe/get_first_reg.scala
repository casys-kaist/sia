package qrd_pe

import chisel3._
import chisel3.util._
import floats.FloatType

/*
 *  GetFirstReg: Always return first element of the input vector
 *      - 1 cycle
 */

class GetFirstReg extends Module {
    val io = IO(new Bundle{
        val input = Flipped(new Valid(new FloatType(8, 23)))
        val output = new Valid(new FloatType(8, 23))
    })

    val init_value = Wire(new Valid(new FloatType(8, 23)))
    init_value.valid := 0.U
    init_value.bits.sign := 0.U(1.W)
    init_value.bits.exp := 0.U(8.W)
    init_value.bits.frac := 0.U(23.W)

    val reg = RegInit(init_value)
    when((io.input.valid === 1.U) & (reg.valid === 0.U)) {
        reg := io.input
    } .otherwise {
        reg := reg
    }

    io.output := reg
}
