package helpers

import chisel3._
import floats.FloatType
import scala.util.Random

object HelperFunctions {
    def ChiselFloat_to_ScalaFloat (input: FloatType) : Float = {
        if (input.sign.litValue == 0) {
            val strings = input.litValue.toString
            val intBits = Integer.parseInt(strings, 10)
            return java.lang.Float.intBitsToFloat(intBits)
        } else {
            val binary_long = input.litValue - 2147483648L
            val strings = binary_long.toString
            val intBits = Integer.parseInt(strings, 10)
            return -1 * java.lang.Float.intBitsToFloat(intBits)
        }
    }

    def ScalaFloat_to_ChiselFloat (input: Float) : UInt = {
        if (input < 0) {
            val intBits = java.lang.Float.floatToIntBits(-1 * input)
            return (intBits + 2147483648L).U(32.W)
        } else {
            val intBits = java.lang.Float.floatToIntBits(input)
            return intBits.U(32.W)
        }
    }

    def Random_gen() : Float = {
        val random = new Random()
        val min = -100.0f
        val max = 500.0f
        min + (max - min) * random.nextFloat()
    }
}