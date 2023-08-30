package floats

import chisel3._
import chiseltest._
import org.scalatest.flatspec.AnyFlatSpec
import helpers.HelperFunctions

class TestFloatSqrt() extends AnyFlatSpec with ChiselScalatestTester {
    /*"FloatSqrt" should "pass" in {
        test(new FloatSqrt(8)) { dut =>
            val chiselFP = HelperFunctions.ScalaFloat_to_ChiselFloat(3141.592f)

            dut.io.input.sign.poke(chiselFP(31))
            dut.io.input.exp.poke(chiselFP(30, 23))
            dut.io.input.frac.poke(chiselFP(22, 0))

            for (i <- 1 to 10) {
                dut.clock.step(1)

                val result = dut.io.output.peek()
                val float = HelperFunctions.ChiselFloat_to_ScalaFloat(result)
                val inputfloat = HelperFunctions.ChiselFloat_to_ScalaFloat(dut.io.input.peek())
                //println("debug line: " + dut.io.debug_output.peek().toString)
                //println("Result: " + result.toString + " sqrt( " + inputfloat + " ) = " + float)
            }
        }
    }*/
}