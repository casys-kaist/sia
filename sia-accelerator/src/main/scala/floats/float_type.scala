package floats

import chisel3._

/*
 * FloatType: Wrapper for floating point values
 */

class FloatType(exp_bits: Int, frac_bits: Int) extends Bundle {
    val sign = UInt(1.W)
    val exp = UInt(exp_bits.W)
    val frac = UInt(frac_bits.W)
}