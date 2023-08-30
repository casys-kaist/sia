package floats

import chisel3._
import chisel3.util._

/*
 * FloatAdd: floating point adder
 *      cycles: 4
 */

class FloatAdd extends Module {
    val io = IO(new Bundle{
        val input_a = Input(new FloatType(8, 23))
        val input_b = Input(new FloatType(8, 23))
        val output = Output(new FloatType(8, 23))
    })

    val stage1 = Module(new FloatAdd_stage1)
    val stage2 = Module(new FloatAdd_stage2)
    val stage3 = Module(new FloatAdd_stage3)
    val stage4 = Module(new FloatAdd_stage4)

    stage1.io.input_a := io.input_a
    stage1.io.input_b := io.input_b

    stage2.io.bigger_mant_12 := stage1.io.bigger_mant_12
    stage2.io.smaller_mant_12 := stage1.io.smaller_mant_12
    stage2.io.exp_12 := stage1.io.exp_12
    stage2.io.sign_12 := stage1.io.sign_12
    stage2.io.need_sub_12 := stage1.io.need_sub_12
    stage2.io.diff_half_12 := stage1.io.diff_half_12

    stage3.io.sign_23 := stage2.io.sign_23
    stage3.io.exp_23 := stage2.io.exp_23
    stage3.io.mant_23 := stage2.io.mant_23
    stage3.io.need_sub_23 := stage2.io.need_sub_23

    stage4.io.sign_34 := stage3.io.sign_34
    stage4.io.exp_34 := stage3.io.exp_34
    stage4.io.mant_34 := stage3.io.mant_34
    
    io.output := stage4.io.output
}

class FloatAdd_stage1 extends Module {
    val io = IO(new Bundle {
        val input_a = Input(new FloatType(8, 23))
        val input_b = Input(new FloatType(8, 23))
        val bigger_mant_12 = Output(UInt(24.W))
        val smaller_mant_12 = Output(UInt(24.W))
        val exp_12 = Output(UInt(8.W))
        val sign_12 = Output(UInt(1.W))
        val need_sub_12 = Output(UInt(1.W))
        val diff_half_12 = Output(UInt(4.W))
    })

    /*
        First stage of FP adder:
            Compare two exponents a_exp and b_exp.
            if b_exp is bigger than a_exp, swap a_frac and b_frac
     */

    val comp = (io.input_a.exp > io.input_b.exp) | ((io.input_a.exp === io.input_b.exp) & (io.input_a.frac >= io.input_b.frac))
    val exp_diff = Mux(comp, io.input_a.exp - io.input_b.exp, io.input_b.exp - io.input_a.exp).asTypeOf(UInt(5.W))
    val exp_diff_first_half = (exp_diff >> 1).asTypeOf(UInt(4.W))
    val exp_diff_second_half = ((exp_diff >> 1) + exp_diff(0)).asTypeOf(UInt(4.W))

    val mant_a = Mux(io.input_a.exp === 0.U, 0.U(1.W), 1.U(1.W)) ## io.input_a.frac // If exp == 0, the number is denormalized one, so mantissa is 0.XXX
    val mant_b = Mux(io.input_b.exp === 0.U, 0.U(1.W), 1.U(1.W)) ## io.input_b.frac // Else, mantissa is 1.XXX

    val first_input = Mux(comp, mant_a, mant_b) // bigger one
    val second_input = Mux(comp, mant_b, mant_a) // smaller one
    val second_input_shifted = second_input >> exp_diff_first_half

    // Pipelined registers
    val reg_bigger_mant = RegNext(first_input, 0.U)
    val reg_smaller_mant = RegNext(second_input_shifted, 0.U)
    val reg_exp = RegNext(Mux(comp, io.input_a.exp, io.input_b.exp), 0.U)
    val reg_sign = RegNext(Mux(comp, io.input_a.sign, io.input_b.sign), 0.U)
    val reg_need_sub = RegNext(io.input_a.sign ^ io.input_b.sign, 0.U)
    val reg_diff_half = RegNext(exp_diff_second_half)

    io.bigger_mant_12 := reg_bigger_mant
    io.smaller_mant_12 := reg_smaller_mant
    io.exp_12 := reg_exp
    io.sign_12 := reg_sign
    io.need_sub_12 := reg_need_sub
    io.diff_half_12 := reg_diff_half
}

class FloatAdd_stage2 extends Module {
    val io = IO(new Bundle {
        val bigger_mant_12 = Input(UInt(24.W))
        val smaller_mant_12 = Input(UInt(24.W))
        val exp_12 = Input(UInt(8.W))
        val sign_12 = Input(UInt(1.W))
        val need_sub_12 = Input(UInt(1.W))
        val diff_half_12 = Input(UInt(4.W))
        val mant_23 = Output(UInt(25.W))
        val exp_23 = Output(UInt(8.W))
        val sign_23 = Output(UInt(1.W))
        val need_sub_23 = Output(UInt(1.W))
    })

    /*
        Second stage of FP adder:
            Do integer addition between two mants
    */

    val extended_bigger_mant = 0.U(1.W) ## io.bigger_mant_12
    val extended_smaller_mant = 0.U(1.W) ## (io.smaller_mant_12 >> io.diff_half_12)
    val mant = Mux(io.need_sub_12.asBool, extended_bigger_mant - extended_smaller_mant, extended_bigger_mant + extended_smaller_mant)

    // Pipelined registers
    val reg_mant = RegNext(mant, 0.U)
    val reg_exp = RegNext(io.exp_12, 0.U)
    val reg_sign = RegNext(io.sign_12, 0.U)
    val reg_need_sub = RegNext(io.need_sub_12, 0.U)

    io.mant_23 := reg_mant
    io.exp_23 := reg_exp
    io.sign_23 := reg_sign
    io.need_sub_23 := reg_need_sub
}

class FloatAdd_stage3 extends Module {
    val io = IO(new Bundle {
        val mant_23 = Input(UInt(25.W))
        val exp_23 = Input(UInt(8.W))
        val sign_23 = Input(UInt(1.W))
        val need_sub_23 = Input(UInt(1.W))
        val mant_34 = Output(UInt(24.W))
        val exp_34 = Output(UInt(8.W))
        val sign_34 = Output(UInt(1.W))
    })

    /*
        Third stage of FP adder:
            Adjust mant and exp if there is overflow
    */

    val is_overflow = io.mant_23(24)

    val sign_adj = WireDefault(0.U(1.W))
    val exp_adj = WireDefault(0.U(8.W))
    val mant_adj = WireDefault(0.U(24.W))
    when (is_overflow.asBool) { // If overflow happens
        when (io.need_sub_23.asBool) {  // If it was subtraction
            mant_adj := -io.mant_23(23, 0)
            sign_adj := -io.sign_23
            exp_adj := io.exp_23
        } .otherwise {
            mant_adj := io.mant_23(24, 1) // Shift by 1
            sign_adj := io.sign_23
            exp_adj := io.exp_23 + 1.U
        }
    } .otherwise {
        sign_adj := io.sign_23
        mant_adj := io.mant_23(23, 0)
        exp_adj := io.exp_23
    }

    // Pipelined registers
    val reg_sign = RegNext(sign_adj, 0.U)
    val reg_exp = RegNext(exp_adj, 0.U)
    val reg_mant = RegNext(mant_adj, 0.U)

    io.mant_34 := reg_mant
    io.exp_34 := reg_exp
    io.sign_34 := reg_sign
}

class FloatAdd_stage4 extends Module {
    val io = IO(new Bundle {
        val mant_34 = Input(UInt((24).W))
        val exp_34 = Input(UInt(8.W))
        val sign_34 = Input(UInt(1.W))
        val output = Output(new FloatType(8, 23))
    })

    val norm_shift = PriorityEncoder(Reverse(io.mant_34))

    val mant_norm = WireDefault(0.U(23.W))
    val exp_norm = WireDefault(0.U(8.W))
    when (io.mant_34 === 0.U) {
        mant_norm := 0.U
        exp_norm := 0.U
    } .otherwise {
        mant_norm := (io.mant_34 << norm_shift)(22, 0)
        exp_norm := io.exp_34 - norm_shift
    }

    // Pipelined registers
    val reg_sign = RegNext(io.sign_34, 0.U)
    val reg_exp = RegNext(exp_norm, 0.U)
    val reg_frac = RegNext(mant_norm, 0.U)

    io.output.sign := reg_sign
    io.output.exp := reg_exp
    io.output.frac := reg_frac
}

object FloatAdd extends App {
    (new chisel3.stage.ChiselStage).emitVerilog(new FloatAdd, Array("--target-dir", "generated/floats"))
}