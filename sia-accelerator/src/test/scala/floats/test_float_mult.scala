package floats

import chisel3._
import chiseltest._
import org.scalatest.flatspec.AnyFlatSpec
import helpers.HelperFunctions

class TestFloatMult() extends AnyFlatSpec with ChiselScalatestTester {
    "FloatMult" should "pass" in {
        test(new FloatMult) { dut =>
            val scala_a = 762.291648f
            val scala_b = -0.0013118337f
            val chisel_a = HelperFunctions.ScalaFloat_to_ChiselFloat(scala_a)
            val chisel_b = HelperFunctions.ScalaFloat_to_ChiselFloat(scala_b)

            dut.io.input_a.sign.poke(chisel_a(31))
            dut.io.input_a.exp.poke(chisel_a(30, 23))
            dut.io.input_a.frac.poke(chisel_a(22, 0))
            dut.io.input_b.sign.poke(chisel_b(31))
            dut.io.input_b.exp.poke(chisel_b(30, 23))
            dut.io.input_b.frac.poke(chisel_b(22, 0))

            for (i <- 1 to 10) {
                dut.clock.step(1)
                val result_bit = dut.io.output.peek()
                val result_float = HelperFunctions.ChiselFloat_to_ScalaFloat(result_bit)
                println("Result: " + result_float.toString)
            }
        }
    }
}