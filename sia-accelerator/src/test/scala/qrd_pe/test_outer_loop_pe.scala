package qrd_pe

import chisel3._
import chiseltest._
import org.scalatest.flatspec.AnyFlatSpec
import helpers.HelperFunctions

class TestOuterLoopPE() extends AnyFlatSpec with ChiselScalatestTester {
    "OuterLoopPE" should "pass" in {
        test(new OuterLoopPE(8, 256)) { dut =>
            val scalaFP_vec = Array(2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f,
                                    1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f,
                                    9.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f
                                    )
            val chiselFP_array: Array[UInt] = scalaFP_vec.map(HelperFunctions.ScalaFloat_to_ChiselFloat)
            // Gamma must be: -0.00131189847288103834198824496736 / -0.0013118337 (fpga result)
            // u_k(1) must be: 28.627053911388694974442847144631 / 28.645824 (fpga result)

            // First 8 elements
            dut.io.input.valid.poke(1.U(1.W))
            for (i <- 0 to 7) {
                dut.io.input.bits(i).sign.poke(chiselFP_array(i)(31))
                dut.io.input.bits(i).exp.poke(chiselFP_array(i)(30, 23))
                dut.io.input.bits(i).frac.poke(chiselFP_array(i)(22, 0))
            }
            dut.io.start.valid.poke(1.U(1.W))
            dut.io.start.bits.poke(3.U)

            // Wait for 4 cycles
            for (d <- 1 to 4) {
                dut.clock.step(1)
                dut.io.input.valid.poke(0.U(1.W))
                for (i <- 0 to 7) {
                    dut.io.input.bits(i).sign.poke(0.U(1.W))
                    dut.io.input.bits(i).exp.poke(0.U(8.W))
                    dut.io.input.bits(i).frac.poke(0.U(23.W))
                }
                dut.io.start.valid.poke(0.U(1.W))
                dut.io.start.bits.poke(0.U)
            }

            dut.clock.step(1)
            
            // Second 8 elements
            dut.io.input.valid.poke(1.U(1.W))
            for (i <- 0 to 7) {
                dut.io.input.bits(i).sign.poke(chiselFP_array(i + 8)(31))
                dut.io.input.bits(i).exp.poke(chiselFP_array(i + 8)(30, 23))
                dut.io.input.bits(i).frac.poke(chiselFP_array(i + 8)(22, 0))
            }
            dut.io.start.valid.poke(0.U(1.W))
            dut.io.start.bits.poke(0.U)

            // Wait for 4 cycles
            for (d <- 1 to 4) {
                dut.clock.step(1)
                dut.io.input.valid.poke(0.U(1.W))
                for (i <- 0 to 7) {
                    dut.io.input.bits(i).sign.poke(0.U(1.W))
                    dut.io.input.bits(i).exp.poke(0.U(8.W))
                    dut.io.input.bits(i).frac.poke(0.U(23.W))
                }
                dut.io.start.valid.poke(0.U(1.W))
                dut.io.start.bits.poke(0.U)
            }

            dut.clock.step(1)

            // Third 8 elements
            dut.io.input.valid.poke(1.U(1.W))
            for (i <- 0 to 7) {
                dut.io.input.bits(i).sign.poke(chiselFP_array(i + 16)(31))
                dut.io.input.bits(i).exp.poke(chiselFP_array(i + 16)(30, 23))
                dut.io.input.bits(i).frac.poke(chiselFP_array(i + 16)(22, 0))
            }
            dut.io.start.valid.poke(0.U(1.W))
            dut.io.start.bits.poke(0.U)

            // Last Cycles
            for (d <- 1 to 100) {
                dut.clock.step(1)    
                dut.io.input.valid.poke(0.U(1.W))
                for (i <- 0 to 7) {
                    dut.io.input.bits(i).sign.poke(0.U(1.W))
                    dut.io.input.bits(i).exp.poke(0.U(8.W))
                    dut.io.input.bits(i).frac.poke(0.U(23.W))
                }
                dut.io.start.valid.poke(0.U(1.W))
                dut.io.start.bits.poke(0.U)

                val gamma_valid = dut.io.output_gamma.valid.peek()
                val gamma_bits = dut.io.output_gamma.bits.peek()
                val reflector_valid = dut.io.output_reflector.valid.peek()
                val reflector_bits = dut.io.output_reflector.bits.peek()

                val gamma_float = HelperFunctions.ChiselFloat_to_ScalaFloat(gamma_bits)
                print("Done: " + dut.io.done.peek().toString + " - ")
                print("Gamma: " + gamma_float.toString + " (" + gamma_valid.toString() + ") - Reflector: ")
                for (i <- 0 to 7) {
                    val reflector_float = HelperFunctions.ChiselFloat_to_ScalaFloat(reflector_bits(i))
                    print(reflector_float.toString + " ")
                }
                print("(" + reflector_valid.toString + ")\n")
            }
        }
    }
}

class TestOuterLoopPE_domain1() extends AnyFlatSpec with ChiselScalatestTester {
    "OuterLoopPE_domain1" should "pass" in {
        test(new OuterLoopPE_domain1(8)) { dut =>
            val scalaFP_vec = Array(2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f,
                                    1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f,
                                    9.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f
                                    )
            val chiselFP_array: Array[UInt] = scalaFP_vec.map(HelperFunctions.ScalaFloat_to_ChiselFloat)
            // Gamma must be: -0.00131189847288103834198824496736 / -0.0013118337 (fpga result)
            // u_k(1) must be: 28.627053911388694974442847144631 / 28.645824 (fpga result)

            // First 8 elements
            dut.io.input.valid.poke(1.U(1.W))
            for (i <- 0 to 7) {
                dut.io.input.bits(i).sign.poke(chiselFP_array(i)(31))
                dut.io.input.bits(i).exp.poke(chiselFP_array(i)(30, 23))
                dut.io.input.bits(i).frac.poke(chiselFP_array(i)(22, 0))
            }
            dut.io.start.valid.poke(1.U(1.W))
            dut.io.start.bits.poke(3.U)

            // Wait for 4 cycles
            for (d <- 1 to 4) {
                dut.clock.step(1)
                dut.io.input.valid.poke(0.U(1.W))
                for (i <- 0 to 7) {
                    dut.io.input.bits(i).sign.poke(0.U(1.W))
                    dut.io.input.bits(i).exp.poke(0.U(8.W))
                    dut.io.input.bits(i).frac.poke(0.U(23.W))
                }
                dut.io.start.valid.poke(0.U(1.W))
                dut.io.start.bits.poke(0.U)
            }

            dut.clock.step(1)
            
            // Second 8 elements
            dut.io.input.valid.poke(1.U(1.W))
            for (i <- 0 to 7) {
                dut.io.input.bits(i).sign.poke(chiselFP_array(i + 8)(31))
                dut.io.input.bits(i).exp.poke(chiselFP_array(i + 8)(30, 23))
                dut.io.input.bits(i).frac.poke(chiselFP_array(i + 8)(22, 0))
            }
            dut.io.start.valid.poke(0.U(1.W))
            dut.io.start.bits.poke(0.U)

            // Wait for 4 cycles
            for (d <- 1 to 4) {
                dut.clock.step(1)
                dut.io.input.valid.poke(0.U(1.W))
                for (i <- 0 to 7) {
                    dut.io.input.bits(i).sign.poke(0.U(1.W))
                    dut.io.input.bits(i).exp.poke(0.U(8.W))
                    dut.io.input.bits(i).frac.poke(0.U(23.W))
                }
                dut.io.start.valid.poke(0.U(1.W))
                dut.io.start.bits.poke(0.U)
            }

            dut.clock.step(1)

            // Third 8 elements
            dut.io.input.valid.poke(1.U(1.W))
            for (i <- 0 to 7) {
                dut.io.input.bits(i).sign.poke(chiselFP_array(i + 16)(31))
                dut.io.input.bits(i).exp.poke(chiselFP_array(i + 16)(30, 23))
                dut.io.input.bits(i).frac.poke(chiselFP_array(i + 16)(22, 0))
            }
            dut.io.start.valid.poke(0.U(1.W))
            dut.io.start.bits.poke(0.U)

            // Last Cycles
            for (d <- 1 to 70) {
                dut.clock.step(1)    
                dut.io.input.valid.poke(0.U(1.W))
                for (i <- 0 to 7) {
                    dut.io.input.bits(i).sign.poke(0.U(1.W))
                    dut.io.input.bits(i).exp.poke(0.U(8.W))
                    dut.io.input.bits(i).frac.poke(0.U(23.W))
                }
                dut.io.start.valid.poke(0.U(1.W))
                dut.io.start.bits.poke(0.U)

                val gamma_valid = dut.io.gamma_output.valid.peek()
                val gamma_bits = dut.io.gamma_output.bits.peek()
                val u_k_valid = dut.io.u_k_output.valid.peek()
                val u_k_bits = dut.io.u_k_output.bits.peek()

                val gamma_float = HelperFunctions.ChiselFloat_to_ScalaFloat(gamma_bits)
                val u_k_float = HelperFunctions.ChiselFloat_to_ScalaFloat(u_k_bits)
                println("Gamma Result: " + gamma_float.toString + " (" + gamma_valid.toString + ") " +
                        "u_k(1) Result: " + u_k_float.toString + " (" + u_k_valid.toString + ")")
            }
        }
    }
}

class TestOuterLoopPE_domain2() extends AnyFlatSpec with ChiselScalatestTester {
    "OuterLoopPE_domain2" should "pass" in {
        test(new OuterLoopPE_domain2(8, 64)) { dut =>
            val scalaFP_vec = Array(2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f,
                                    1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f,
                                    9.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f
                                    )
            val chiselFP_array: Array[UInt] = scalaFP_vec.map(HelperFunctions.ScalaFloat_to_ChiselFloat)
            val scalaFP_uk = 99.9f
            val chiselFP_uk = HelperFunctions.ScalaFloat_to_ChiselFloat(scalaFP_uk)

            // Enqueue input vector to the queue
            for (d <- 0 until 3) {
                dut.io.input.valid.poke(true.B)
                for (i <- 0 until 8) {
                    dut.io.input.bits(i).sign.poke(chiselFP_array(d * 8 + i)(31))
                    dut.io.input.bits(i).exp.poke(chiselFP_array(d * 8 + i)(30, 23))
                    dut.io.input.bits(i).frac.poke(chiselFP_array(d * 8 + i)(22, 0))
                }
                dut.io.u_k_input.valid.poke(false.B)
                dut.io.u_k_input.bits.sign.poke(0.U)
                dut.io.u_k_input.bits.exp.poke(0.U)
                dut.io.u_k_input.bits.frac.poke(0.U)
                dut.clock.step(1)
            }

            // Wait for 7 cycle
            for (d <- 1 to 7) {
                dut.io.input.valid.poke(false.B)
                for (i <- 0 until 8) {
                    dut.io.input.bits(i).sign.poke(0.U)
                    dut.io.input.bits(i).exp.poke(0.U)
                    dut.io.input.bits(i).frac.poke(0.U)
                }
                dut.io.u_k_input.valid.poke(false.B)
                dut.io.u_k_input.bits.sign.poke(0.U)
                dut.io.u_k_input.bits.exp.poke(0.U)
                dut.io.u_k_input.bits.frac.poke(0.U)
                dut.clock.step(1)
            }

            // Input u_k(1)
            dut.io.input.valid.poke(false.B)
            for (i <- 0 until 8) {
                dut.io.input.bits(i).sign.poke(0.U)
                dut.io.input.bits(i).exp.poke(0.U)
                dut.io.input.bits(i).frac.poke(0.U)
            }
            dut.io.u_k_input.valid.poke(true.B)
            dut.io.u_k_input.bits.sign.poke(chiselFP_uk(31))
            dut.io.u_k_input.bits.exp.poke(chiselFP_uk(30, 23))
            dut.io.u_k_input.bits.frac.poke(chiselFP_uk(22, 0))

            // Last Cycles
            for (d <- 1 to 30) {
                dut.clock.step(1)
                
                dut.io.input.valid.poke(false.B)
                for (i <- 0 until 8) {
                    dut.io.input.bits(i).sign.poke(0.U)
                    dut.io.input.bits(i).exp.poke(0.U)
                    dut.io.input.bits(i).frac.poke(0.U)
                }
                dut.io.u_k_input.valid.poke(false.B)
                dut.io.u_k_input.bits.sign.poke(0.U)
                dut.io.u_k_input.bits.exp.poke(0.U)
                dut.io.u_k_input.bits.frac.poke(0.U)

                val output_valid = dut.io.output.valid.peek()
                val output_bits = dut.io.output.bits.peek()

                print("Result: (" + output_valid.toString + ") ")
                for (i <- 0 until 8) {
                    print(HelperFunctions.ChiselFloat_to_ScalaFloat(output_bits(i)).toString + ", ")
                }
                print("\n")
            }
        }
    }
}