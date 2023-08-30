package floats

import chisel3._
import chisel3.util._

/*
 * FloatMult: floating point multiplier
 *      cycles: 3
 */

class FloatMult extends Module {
    val io = IO(new Bundle{
        val input_a = Input(new FloatType(8, 23))
        val input_b = Input(new FloatType(8, 23))
        val output = Output(new FloatType(8, 23))
    })

    val stage1 = Module(new FloatMult_stage1)
    val stage2 = Module(new FloatMult_stage2)
    val stage3 = Module(new FloatMult_stage3)

    stage1.io.input_a := io.input_a
    stage1.io.input_b := io.input_b

    stage2.io.exp_12 := stage1.io.exp_12
    stage2.io.sign_12 := stage1.io.sign_12
    stage2.io.mant_a_12 := stage1.io.mant_a_12
    stage2.io.mant_b_12 := stage1.io.mant_b_12
    stage2.io.is_zero_12 := stage1.io.is_zero_12

    stage3.io.exp_23 := stage2.io.exp_23
    stage3.io.sign_23 := stage2.io.sign_23
    stage3.io.mant_23 := stage2.io.mant_23
    stage3.io.is_zero_23 := stage2.io.is_zero_23

    io.output := stage3.io.output
}

class FloatMult_stage1 extends Module {
    val io = IO(new Bundle {
        val input_a = Input(new FloatType(8, 23))
        val input_b = Input(new FloatType(8, 23))

        val exp_12 = Output(UInt(8.W))
        val sign_12 = Output(UInt(1.W))
        val mant_a_12 = Output(UInt(24.W))
        val mant_b_12 = Output(UInt(24.W))
        val is_zero_12 = Output(UInt(1.W))
    })

    /*
        First stage of FP multiplier:
            Add two exponents
            Determine sign bit
    */

    val mant_a = Mux(io.input_a.exp === 0.U, 0.U(1.W), 1.U(1.W)) ## io.input_a.frac // If exp == 0, the number is denormalized one, so mantissa is 0.XXX
    val mant_b = Mux(io.input_b.exp === 0.U, 0.U(1.W), 1.U(1.W)) ## io.input_b.frac // Else, mantissa is 1.XXX

    val sign = io.input_a.sign ^ io.input_b.sign
    val exp = io.input_a.exp + io.input_b.exp
    val is_zero = ((io.input_a.exp === 0.U) & (io.input_a.frac === 0.U)) | 
                  ((io.input_b.exp === 0.U) & (io.input_b.frac === 0.U))
    
    val reg_mant_a = RegNext(mant_a, 0.U)
    val reg_mant_b = RegNext(mant_b, 0.U)
    val reg_sign = RegNext(sign, 0.U)
    val reg_exp = RegNext(exp, 0.U)
    val reg_is_zero = RegNext(is_zero.asUInt, 1.U)

    io.exp_12 := reg_exp
    io.sign_12 := reg_sign
    io.mant_a_12 := reg_mant_a
    io.mant_b_12 := reg_mant_b
    io.is_zero_12 := reg_is_zero
}

class FloatMult_stage2 extends Module {
    val io = IO(new Bundle {
        val exp_12 = Input(UInt(8.W))
        val sign_12 = Input(UInt(1.W))
        val mant_a_12 = Input(UInt(24.W))
        val mant_b_12 = Input(UInt(24.W))
        val is_zero_12 = Input(UInt(1.W))

        val exp_23 = Output(UInt(8.W))
        val sign_23 = Output(UInt(1.W))
        val mant_23 = Output(UInt((23*2+2).W))
        val is_zero_23 = Output(UInt(1.W))
    })

    /*
        Second stage of FP multiplier:
            multiply two mantissas
    */

    val mant_mult = WireDefault(0.U((23*2+2).W))
    mant_mult := io.mant_a_12 * io.mant_b_12 // TODO: Timing Violation Here

    val reg_exp = RegNext(io.exp_12, 0.U)
    val reg_sign = RegNext(io.sign_12, 0.U)
    val reg_mant = RegNext(mant_mult, 0.U)
    val reg_is_zero = RegNext(io.is_zero_12, 1.U)

    io.exp_23 := reg_exp
    io.sign_23 := reg_sign
    io.mant_23 := reg_mant
    io.is_zero_23 := reg_is_zero
}

// Referenced from https://github.com/zhemao/chisel-float/blob/master/src/main/scala/FPMult.scala
class MantissaRounder(val n: Int) extends Module {
    val io = IO(new Bundle {
        val in = Input(UInt(n.W))
        val out = Output(UInt((n - 1).W))
    })

    io.out := io.in(n - 1, 1) + io.in(0)
}

class FloatMult_stage3 extends Module {
    val io = IO(new Bundle {
        val exp_23 = Input(UInt(8.W))
        val sign_23 = Input(UInt(1.W))
        val mant_23 = Input(UInt((23*2+2).W))
        val is_zero_23 = Input(UInt(1.W))

        val output = Output(new FloatType(8, 23))
    })

    /*
        Third stage of FP multiplier:
            Do normalization by modifying exp bits and shifting mant bits
            Rounding
    */

    val rounder = Module(new MantissaRounder(23+1))

    val wire_exp = WireDefault(0.U(8.W))

    when (io.is_zero_23 === 1.U) {
        // Should return zero
        wire_exp := 0.U(8.W)
        rounder.io.in := 0.U((23+1).W)
    } .elsewhen (io.mant_23(23 * 2 + 1) === 1.U(1.W)) {
        wire_exp := io.exp_23 - 126.U
        rounder.io.in := io.mant_23(23 * 2, 23)
    } .otherwise {
        wire_exp := io.exp_23 - 127.U
        rounder.io.in := io.mant_23(23 * 2 - 1, 23 - 1)
    }

    val reg_sign = RegNext(io.sign_23, 0.U)
    val reg_exp = RegNext(wire_exp, 0.U)
    val reg_frac = RegNext(rounder.io.out, 0.U)

    io.output.sign := reg_sign
    io.output.exp := reg_exp
    io.output.frac := reg_frac
}

object FloatMult extends App {
    (new chisel3.stage.ChiselStage).emitVerilog(new FloatMult, Array("--target-dir", "generated/floats"))
}