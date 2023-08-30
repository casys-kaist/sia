package qrd_pe

import chisel3._
import chiseltest._
import org.scalatest.flatspec.AnyFlatSpec
import helpers.HelperFunctions

class TestDotProductTree() extends AnyFlatSpec with ChiselScalatestTester {
    "DotProductTree" should "pass" in {
        test(new DotProductTree(8)) { dut =>
            val scalaFP_vec_a = Array(1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f)
            val scalaFP_vec_b = Array(2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f)
            val chiselFP_array_a: Array[UInt] = scalaFP_vec_a.map(HelperFunctions.ScalaFloat_to_ChiselFloat)
            val chiselFP_array_b: Array[UInt] = scalaFP_vec_b.map(HelperFunctions.ScalaFloat_to_ChiselFloat)

            var sum = 0.0f
            for (i <- 0 to 7) {
                sum += scalaFP_vec_a(i) * scalaFP_vec_b(i)
            }
            println("The answer is " + sum.toString())

            dut.io.input_a.valid.poke(1.U(1.W))
            dut.io.input_b.valid.poke(1.U(1.W))
            for (i <- 0 to 7) {    
                dut.io.input_a.bits(i).sign.poke(chiselFP_array_a(i)(31))
                dut.io.input_a.bits(i).exp.poke(chiselFP_array_a(i)(30, 23))
                dut.io.input_a.bits(i).frac.poke(chiselFP_array_a(i)(22, 0))
                dut.io.input_b.bits(i).sign.poke(chiselFP_array_b(i)(31))
                dut.io.input_b.bits(i).exp.poke(chiselFP_array_b(i)(30, 23))
                dut.io.input_b.bits(i).frac.poke(chiselFP_array_b(i)(22, 0))
            }

            dut.io.start.valid.poke(1.U(1.W))
            dut.io.start.bits.poke(1.U)

            for (i <- 1 to 30) {
                dut.clock.step(1)

                dut.io.input_a.valid.poke(0.U(1.W))
                dut.io.input_b.valid.poke(0.U(1.W))
                for (i <- 0 to 7) {
                    dut.io.input_a.bits(i).sign.poke(0.U(1.W))
                    dut.io.input_a.bits(i).exp.poke(0.U(8.W))
                    dut.io.input_a.bits(i).frac.poke(0.U(23.W))
                    dut.io.input_b.bits(i).sign.poke(0.U(1.W))
                    dut.io.input_b.bits(i).exp.poke(0.U(8.W))
                    dut.io.input_b.bits(i).frac.poke(0.U(23.W))
                }

                dut.io.start.valid.poke(0.U(1.W))
                dut.io.start.bits.poke(0.U)

                val bits_result = dut.io.output.bits.peek()
                val valid_result = dut.io.output.valid.peek()

                val float = HelperFunctions.ChiselFloat_to_ScalaFloat(bits_result)
                //println("debug line: " + dut.io.debug_output.peek().toString)
                println("Result: " + float.toString + " (" + valid_result.toString + ")")
            }
        }
    }
}

class TestThreeDotProductTree() extends AnyFlatSpec with ChiselScalatestTester {
    "3-DotProductTree" should "pass" in {
        test(new DotProductTree(8)) { dut =>
            val scalaFP_vec_a = Array(2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f,
                                    1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f,
                                    9.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f
                                      )
            val scalaFP_vec_b = Array(28.645824f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f,
                                    1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f,
                                    9.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f
                                    )
            val chiselFP_array_a: Array[UInt] = scalaFP_vec_a.map(HelperFunctions.ScalaFloat_to_ChiselFloat)
            val chiselFP_array_b: Array[UInt] = scalaFP_vec_b.map(HelperFunctions.ScalaFloat_to_ChiselFloat)

            var sum = 0.0f
            for (i <- 0 to 23) {
                sum += scalaFP_vec_a(i) * scalaFP_vec_b(i)
            }
            println("The answer is " + sum.toString())

            dut.io.input_a.valid.poke(1.U(1.W))
            dut.io.input_b.valid.poke(1.U(1.W))
            for (i <- 0 to 7) {
                dut.io.input_a.bits(i).sign.poke(chiselFP_array_a(i)(31))
                dut.io.input_a.bits(i).exp.poke(chiselFP_array_a(i)(30, 23))
                dut.io.input_a.bits(i).frac.poke(chiselFP_array_a(i)(22, 0))
                dut.io.input_b.bits(i).sign.poke(chiselFP_array_b(i)(31))
                dut.io.input_b.bits(i).exp.poke(chiselFP_array_b(i)(30, 23))
                dut.io.input_b.bits(i).frac.poke(chiselFP_array_b(i)(22, 0))
            }
            dut.io.start.valid.poke(1.U(1.W))
            dut.io.start.bits.poke(3.U)

            for (d <- 1 to 4) {
                dut.clock.step(1)
                dut.io.input_a.valid.poke(0.U(1.W))
                dut.io.input_b.valid.poke(0.U(1.W))
                 for (i <- 0 to 7) {
                    dut.io.input_a.bits(i).sign.poke(0.U(1.W))
                    dut.io.input_a.bits(i).exp.poke(0.U(8.W))
                    dut.io.input_a.bits(i).frac.poke(0.U(23.W))
                    dut.io.input_b.bits(i).sign.poke(0.U(1.W))
                    dut.io.input_b.bits(i).exp.poke(0.U(8.W))
                    dut.io.input_b.bits(i).frac.poke(0.U(23.W))
                }
                dut.io.start.valid.poke(0.U(1.W))
                dut.io.start.bits.poke(0.U)
            }

            dut.clock.step(1)

            dut.io.input_a.valid.poke(1.U(1.W))
            dut.io.input_b.valid.poke(1.U(1.W))
            for (i <- 0 to 7) {
                dut.io.input_a.bits(i).sign.poke(chiselFP_array_a(i + 8)(31))
                dut.io.input_a.bits(i).exp.poke(chiselFP_array_a(i + 8)(30, 23))
                dut.io.input_a.bits(i).frac.poke(chiselFP_array_a(i + 8)(22, 0))
                dut.io.input_b.bits(i).sign.poke(chiselFP_array_b(i + 8)(31))
                dut.io.input_b.bits(i).exp.poke(chiselFP_array_b(i + 8)(30, 23))
                dut.io.input_b.bits(i).frac.poke(chiselFP_array_b(i + 8)(22, 0))
            }
            dut.io.start.valid.poke(0.U(1.W))
            dut.io.start.bits.poke(0.U)

            for (d <- 1 to 4) {
                dut.clock.step(1)
                dut.io.input_a.valid.poke(0.U(1.W))
                dut.io.input_b.valid.poke(0.U(1.W))
                for (i <- 0 to 7) {
                    dut.io.input_a.bits(i).sign.poke(0.U(1.W))
                    dut.io.input_a.bits(i).exp.poke(0.U(8.W))
                    dut.io.input_a.bits(i).frac.poke(0.U(23.W))
                    dut.io.input_b.bits(i).sign.poke(0.U(1.W))
                    dut.io.input_b.bits(i).exp.poke(0.U(8.W))
                    dut.io.input_b.bits(i).frac.poke(0.U(23.W))
                }
                dut.io.start.valid.poke(0.U(1.W))
                dut.io.start.bits.poke(0.U)
            }

            dut.clock.step(1)

            dut.io.input_a.valid.poke(1.U(1.W))
            dut.io.input_b.valid.poke(1.U(1.W))
            for (i <- 0 to 7) {
                dut.io.input_a.bits(i).sign.poke(chiselFP_array_a(i + 16)(31))
                dut.io.input_a.bits(i).exp.poke(chiselFP_array_a(i + 16)(30, 23))
                dut.io.input_a.bits(i).frac.poke(chiselFP_array_a(i + 16)(22, 0))
                dut.io.input_b.bits(i).sign.poke(chiselFP_array_b(i + 16)(31))
                dut.io.input_b.bits(i).exp.poke(chiselFP_array_b(i + 16)(30, 23))
                dut.io.input_b.bits(i).frac.poke(chiselFP_array_b(i + 16)(22, 0))
            }
            dut.io.start.valid.poke(0.U(1.W))
            dut.io.start.bits.poke(0.U)

            for (d <- 1 to 30) {
                dut.clock.step(1)
                dut.io.input_a.valid.poke(0.U(1.W))
                dut.io.input_b.valid.poke(0.U(1.W))
                for (i <- 0 to 7) {
                    dut.io.input_a.bits(i).sign.poke(0.U(1.W))
                    dut.io.input_a.bits(i).exp.poke(0.U(8.W))
                    dut.io.input_a.bits(i).frac.poke(0.U(23.W))
                    dut.io.input_b.bits(i).sign.poke(0.U(1.W))
                    dut.io.input_b.bits(i).exp.poke(0.U(8.W))
                    dut.io.input_b.bits(i).frac.poke(0.U(23.W))
                }
                dut.io.start.valid.poke(0.U(1.W))
                dut.io.start.bits.poke(0.U)

                val bits_result = dut.io.output.bits.peek()
                val valid_result = dut.io.output.valid.peek()

                val float = HelperFunctions.ChiselFloat_to_ScalaFloat(bits_result)
                //println("debug line: " + dut.io.debug_output.peek().toString)
                println("Result: " + float.toString + " (" + valid_result.toString + ")")
            }
        }
    }
}
