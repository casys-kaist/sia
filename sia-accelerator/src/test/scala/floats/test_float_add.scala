package floats

import chisel3._
import chiseltest._
import org.scalatest.flatspec.AnyFlatSpec
import helpers.HelperFunctions

class TestFloatAdd() extends AnyFlatSpec with ChiselScalatestTester {
    "FloatAdd" should "pass" in {
        test(new FloatAdd) { dut =>
            val chiselFP_a = HelperFunctions.ScalaFloat_to_ChiselFloat(3.2f)
            val chiselFP_b = HelperFunctions.ScalaFloat_to_ChiselFloat(372.56f)

            dut.io.input_a.sign.poke(chiselFP_a(31))
            dut.io.input_a.exp.poke(chiselFP_a(30, 23))
            dut.io.input_a.frac.poke(chiselFP_a(22, 0))
            dut.io.input_b.sign.poke(chiselFP_b(31))
            dut.io.input_b.exp.poke(chiselFP_b(30, 23))
            dut.io.input_b.frac.poke(chiselFP_b(22, 0))

            for (i <- 1 to 10) {
                dut.clock.step(1)

                val result = dut.io.output.peek()
                val float = HelperFunctions.ChiselFloat_to_ScalaFloat(result)
                val inputfloat_a = HelperFunctions.ChiselFloat_to_ScalaFloat(dut.io.input_a.peek())
                val inputfloat_b = HelperFunctions.ChiselFloat_to_ScalaFloat(dut.io.input_b.peek())
                //println("debug line: " + dut.io.debug_output.peek().toString)
                println("Result: " + result.toString + " ( " + inputfloat_a + " + " + inputfloat_b + " ) = " + float)
            }
        }
    }
}