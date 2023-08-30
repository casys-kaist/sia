package floats

import chisel3._
import chisel3.util._

/*
 * FloatOneDiv: floating point divider
 *      - The diviend is always -2 and input becomes the divider
 *      cycles: 26
 */

class FloatOneDiv extends Module {
    val io = IO(new Bundle {
        val input = Input(new FloatType(8, 23))
        val output = Output(new FloatType(8, 23))
    })

    val stage_head = Module(new FloatOneDiv_head)
    val stage_tail = Module(new FloatOneDiv_tail)
    val stage_bodies = Array.tabulate(23+1)(i => Module(new FloatOneDiv_body(23-i)))

    stage_head.io.input := io.input
    stage_bodies(0).io.input := stage_head.io.output
    for (i <- 1 to 23) {
        stage_bodies(i).io.input := stage_bodies(i-1).io.output
    }
    stage_tail.io.input := stage_bodies(23).io.output
    io.output := stage_tail.io.output
}

class FloatOneDiv_Bundle extends Bundle {
    val diviend = UInt((23*2+2).W)
    val divider = UInt((23*2+2).W)
    val quotient = UInt((23+1).W)
    val is_nan = UInt(1.W)
    val sign = UInt(1.W)
    val exp = UInt((8).W)
}

class FloatOneDiv_head extends Module {
    val io = IO(new Bundle {
        val input = Input(new FloatType(8, 23))
        val output = Output(new FloatOneDiv_Bundle)
    })

    val sign = 1.U(1.W) ^ io.input.sign
    val exp = 254.U((8).W) - io.input.exp
    val is_nan = (io.input.exp === 0.U) & (io.input.frac === 0.U)

    val divider = (Mux(io.input.exp === 0.U, 0.U(1.W), 1.U(1.W)) ## io.input.frac) << (23 + 1).U
    val diviend = 8388608.U << (23 + 1).U

    val reg_sign = RegNext(sign, 0.U)
    val reg_exp = RegNext(exp, 0.U)
    val reg_divider = RegNext(divider, 1.U)
    val reg_diviend = RegNext(diviend, 0.U)
    val reg_is_nan = RegNext(is_nan, 1.U)
    val reg_quotient = RegNext(0.U, 0.U)

    io.output.diviend := reg_diviend
    io.output.divider := reg_divider
    io.output.is_nan := reg_is_nan
    io.output.sign := reg_sign
    io.output.exp := reg_exp
    io.output.quotient := reg_quotient
}

class FloatOneDiv_body(position: Int) extends Module {
    val io = IO(new Bundle {
        val input = Input(new FloatOneDiv_Bundle)
        val output = Output(new FloatOneDiv_Bundle)
    })

    // Bypass
    val reg_is_nan = RegNext(io.input.is_nan, 1.U)
    val reg_sign = RegNext(io.input.sign, 0.U)
    val reg_exp = RegNext(io.input.exp, 0.U)
    io.output.is_nan := reg_is_nan
    io.output.sign := reg_sign
    io.output.exp := reg_exp

    // Calc
    val diviend = WireDefault(0.U)
    val divider = WireDefault(1.U)
    val quotient = WireDefault(0.U)

    when (io.input.is_nan === 1.U) {
        diviend := 0.U
        divider := 1.U
        quotient := 0.U
    } .otherwise {
        // Compare diviend and divider
        val cmp_result = Mux((io.input.diviend >= io.input.divider), 1.U, 0.U)

        // If diviend is larger than divider, subtract & add 1 to quotient
        when (cmp_result === 1.U) {
            quotient := io.input.quotient | (1.U << position)
            diviend := io.input.diviend - io.input.divider
        } .otherwise {
            quotient := io.input.quotient
            diviend := io.input.diviend
        }

        // Else, bypass values
        divider := io.input.divider >> 1
    }

    val reg_diviend = RegNext(diviend, 0.U)
    val reg_divider = RegNext(divider, 1.U)
    val reg_quotient = RegNext(quotient, 0.U)

    io.output.diviend := reg_diviend
    io.output.divider := reg_divider
    io.output.quotient := reg_quotient
}

class FloatOneDiv_tail extends Module {
    val io = IO(new Bundle {
        val input = Input(new FloatOneDiv_Bundle)
        val output = Output(new FloatType(8, 23))
    })
    val sign = WireDefault(0.U)
    val exp = WireDefault(0.U)
    val frac = WireDefault(0.U)

    when (io.input.is_nan === 1.U) {
        // When "divided by 0", return inf
        frac := 0.U
        exp := 255.U
    } .otherwise {
        when (io.input.quotient(23) === 0.U) {
            frac := (io.input.quotient << 1)(23 - 1, 0)
            exp := io.input.exp - 1.U
        } .otherwise {
            frac := io.input.quotient
            exp := io.input.exp
        }
    }

    sign := io.input.sign

    val reg_sign = RegNext(sign, 0.U)
    val reg_exp = RegNext(exp, 0.U)
    val reg_frac = RegNext(frac, 0.U)

    io.output.sign := reg_sign
    io.output.exp := reg_exp
    io.output.frac := reg_frac
}

object FloatOneDiv extends App {
    (new chisel3.stage.ChiselStage).emitVerilog(new FloatOneDiv, Array("--target-dir", "generated/floats"))
}