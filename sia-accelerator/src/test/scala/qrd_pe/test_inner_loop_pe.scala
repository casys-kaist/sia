package qrd_pe

import chisel3._
import chiseltest._
import org.scalatest.flatspec.AnyFlatSpec
import helpers.HelperFunctions

class TestInnerLoopPE() extends AnyFlatSpec with ChiselScalatestTester {
    "InnerLoopPE" should "pass" in {
        test(new InnerLoopPE(8, 256)) { dut =>
            def dut_output() = {
                val vector_valid = dut.io.output.valid.peek()
                val vector_bits = dut.io.output.bits.peek()
                
                print("Done: " + dut.io.done.peek().toString + " - ")
                for (i <- 0 to 7) {
                    val vector_float = HelperFunctions.ChiselFloat_to_ScalaFloat(vector_bits(i))
                    print(vector_float.toString + " ")
                }
                print("(" + vector_valid.toString + ")\n")
            }
        
            def dut_input(
                input_a_valid: Bool,
                input_a_bits: Array[UInt],
                input_u_valid: Bool,
                input_u_bits: Array[UInt],
                input_gamma_valid: Bool,
                input_gamma_bits: UInt,
                start_valid: Bool,
                start_bits: UInt
            ) = {
                dut.io.input_a.valid.poke(input_a_valid)
                dut.io.input_u.valid.poke(input_u_valid)
                for (i <- 0 to 7) {
                    dut.io.input_a.bits(i).sign.poke(input_a_bits(i)(31))
                    dut.io.input_a.bits(i).exp.poke(input_a_bits(i)(30, 23))
                    dut.io.input_a.bits(i).frac.poke(input_a_bits(i)(22, 0))
                    dut.io.input_u.bits(i).sign.poke(input_u_bits(i)(31))
                    dut.io.input_u.bits(i).exp.poke(input_u_bits(i)(30, 23))
                    dut.io.input_u.bits(i).frac.poke(input_u_bits(i)(22, 0))
                }
                dut.io.input_gamma.valid.poke(input_gamma_valid)
                dut.io.input_gamma.bits.sign.poke(input_gamma_bits(31))
                dut.io.input_gamma.bits.exp.poke(input_gamma_bits(30, 23))
                dut.io.input_gamma.bits.frac.poke(input_gamma_bits(22, 0))
                dut.io.start.valid.poke(start_valid)
                dut.io.start.bits.poke(start_bits)
                dut_output()
            }

            val scalaFP_a_vec = Array(2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f,
                                    1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f,
                                    9.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f
                                    )
            val scalaFP_u_vec = Array(28.645824f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f,
                                    1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f,
                                    9.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f
                                    )
            val scalaFP_gamma = -0.0013118337f
            val chiselFP_a_vec = scalaFP_a_vec.map(HelperFunctions.ScalaFloat_to_ChiselFloat)
            val chiselFP_u_vec = scalaFP_u_vec.map(HelperFunctions.ScalaFloat_to_ChiselFloat)
            val chiselFP_gamma = HelperFunctions.ScalaFloat_to_ChiselFloat(scalaFP_gamma)

            for (x <- 0 to 2) {
                // First 8 elements
                dut.io.reset.poke(false.B)
                dut_input(true.B, chiselFP_a_vec.slice(0, 8), true.B, chiselFP_u_vec.slice(0, 8),
                        true.B, chiselFP_gamma, true.B, 3.U)
                dut.clock.step(1)

                // Wait for 4 cycles
                for (i <- 0 until 4) {
                    dut_input(false.B, Array.fill(8)(0.U), false.B, Array.fill(8)(0.U),
                            false.B, 0.U, false.B, 0.U)
                    dut.clock.step(1)
                }

                // Second 8 elements
                dut_input(true.B, chiselFP_a_vec.slice(8, 16), true.B, chiselFP_u_vec.slice(8, 16),
                        true.B, chiselFP_gamma, true.B, 3.U)
                dut.clock.step(1)

                // Wait for 4 cycles
                for (i <- 0 until 4) {
                    dut_input(false.B, Array.fill(8)(0.U), false.B, Array.fill(8)(0.U),
                            false.B, 0.U, false.B, 0.U)
                    dut.clock.step(1)
                }

                // Third 8 elements
                dut_input(true.B, chiselFP_a_vec.slice(16, 24), true.B, chiselFP_u_vec.slice(16, 24),
                        true.B, chiselFP_gamma, true.B, 3.U)
                dut.clock.step(1)

                // Last Cycles
                while(dut.io.done.peek().litValue != 1) {
                    dut_input(false.B, Array.fill(8)(0.U), false.B, Array.fill(8)(0.U),
                            false.B, 0.U, false.B, 0.U)
                    dut.clock.step(1)
                }
                dut_input(false.B, Array.fill(8)(0.U), false.B, Array.fill(8)(0.U),
                            false.B, 0.U, false.B, 0.U)
                dut.clock.step(1)
                dut.io.reset.poke(true.B)
                dut.clock.step(1)
                println("Go to next step")
            }
        }
    }
}

class TestInnerLoopPE_domain1() extends AnyFlatSpec with ChiselScalatestTester {
    "InnerLoopPE_domain1" should "pass" in {
        test(new InnerLoopPE_domain1(8)) { dut =>
            def print_output() = {
                val output_valid = dut.io.output.valid.peek()
                val output_bits = dut.io.output.bits.peek()
                val output_float = HelperFunctions.ChiselFloat_to_ScalaFloat(output_bits)
                println("Result: " + output_float.toString() + " (" + output_valid.toString() + ")")
            }

            def dut_input(
                start_valid : Bool,
                start_bits: UInt,
                input_u_valid: Bool,
                input_u_bits: Array[UInt],
                input_a_valid: Bool,
                input_a_bits: Array[UInt],
                input_gamma_valid: Bool,
                input_gamma_bits: UInt
            ) = {
                dut.io.input_a.valid.poke(input_a_valid)
                dut.io.input_u.valid.poke(input_u_valid)
                for (i <- 0 to 7) {
                    dut.io.input_a.bits(i).sign.poke(input_a_bits(i)(31))
                    dut.io.input_a.bits(i).exp.poke(input_a_bits(i)(30, 23))
                    dut.io.input_a.bits(i).frac.poke(input_a_bits(i)(22, 0))
                    dut.io.input_u.bits(i).sign.poke(input_u_bits(i)(31))
                    dut.io.input_u.bits(i).exp.poke(input_u_bits(i)(30, 23))
                    dut.io.input_u.bits(i).frac.poke(input_u_bits(i)(22, 0))
                }
                dut.io.input_gamma.valid.poke(input_gamma_valid)
                dut.io.input_gamma.bits.sign.poke(input_gamma_bits(31))
                dut.io.input_gamma.bits.exp.poke(input_gamma_bits(30, 23))
                dut.io.input_gamma.bits.frac.poke(input_gamma_bits(22, 0))
                dut.io.start.valid.poke(start_valid)
                dut.io.start.bits.poke(start_bits)
                print_output()
            }

            val scalaFP_a_vec = Array(2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f,
                                    1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f,
                                    9.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f
                                    )
            val scalaFP_u_vec = Array(28.645824f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f,
                                    1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f,
                                    9.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f
                                    )
            val scalaFP_gamma = -0.0013118337f
            val chiselFP_a_vec = scalaFP_a_vec.map(HelperFunctions.ScalaFloat_to_ChiselFloat)
            val chiselFP_u_vec = scalaFP_u_vec.map(HelperFunctions.ScalaFloat_to_ChiselFloat)
            val chiselFP_gamma = HelperFunctions.ScalaFloat_to_ChiselFloat(scalaFP_gamma)

            for (x <- 0 until 3) {
                dut.reset.poke(false.B)
                // First 8 elements
                dut_input(
                    true.B, 3.U,
                    true.B, chiselFP_u_vec.slice(0, 8),
                    true.B, chiselFP_a_vec.slice(0, 8),
                    false.B, 0.U
                )
                dut.clock.step(1)

                // Wait for 4 cycles
                for (d <- 0 until 4) {
                    dut_input(
                        false.B, 0.U,
                        false.B, Array.fill(8)(0.U),
                        false.B, Array.fill(8)(0.U),
                        false.B, 0.U
                    )
                    dut.clock.step(1)
                }

                // Second 8 elements
                dut_input(
                    false.B, 0.U,
                    true.B, chiselFP_u_vec.slice(8, 16),
                    true.B, chiselFP_a_vec.slice(8, 16),
                    false.B, 0.U
                )
                dut.clock.step(1)

                // Wait for 4 cycles
                for (d <- 0 until 4) {
                    dut_input(
                        false.B, 0.U,
                        false.B, Array.fill(8)(0.U),
                        false.B, Array.fill(8)(0.U),
                        false.B, 0.U
                    )
                    dut.clock.step(1)
                }

                // Third 8 elements
                dut_input(
                    false.B, 0.U,
                    true.B, chiselFP_u_vec.slice(16, 24),
                    true.B, chiselFP_a_vec.slice(16, 24),
                    false.B, 0.U
                )
                dut.clock.step(1)

                // Wait for 4 cycles
                for (d <- 0 until 4) {
                    dut_input(
                        false.B, 0.U,
                        false.B, Array.fill(8)(0.U),
                        false.B, Array.fill(8)(0.U),
                        false.B, 0.U
                    )
                    dut.clock.step(1)
                }

                // Input gamma value
                dut_input(
                    false.B, 0.U,
                    false.B, Array.fill(8)(0.U),
                    false.B, Array.fill(8)(0.U),
                    true.B, chiselFP_gamma
                )
                dut.clock.step(1)
                
                // Last Cycles
                while(dut.io.output.valid.peek().litValue != 1) {
                    dut_input(
                        false.B, 0.U,
                        false.B, Array.fill(8)(0.U),
                        false.B, Array.fill(8)(0.U),
                        false.B, 0.U
                    )
                    dut.clock.step(1)
                }
                dut_input(
                    false.B, 0.U,
                    false.B, Array.fill(8)(0.U),
                    false.B, Array.fill(8)(0.U),
                    false.B, 0.U
                )
                dut.clock.step(1)
                dut.reset.poke(true.B)
                dut.clock.step(1)
                println("Module Reset: Go to the next step")
            }
        }
    }
}

class TestInnerLoopPE_domain2() extends AnyFlatSpec with ChiselScalatestTester {
    "InnerLoopPE_domain2" should "pass" in {
        test(new InnerLoopPE_domain2(8, 64)) { dut =>
            def print_output() = {
                val output_valid = dut.io.output.valid.peek()
                val output_floats = dut.io.output.bits.peek()
                print("Output: (" + output_valid.toString + ") - ")
                for (i <- 0 to 7) {
                    val output_float = HelperFunctions.ChiselFloat_to_ScalaFloat(output_floats(i))
                    print(output_float.toString + " ")
                }
                print("\n")
            }

            def dut_input(
                input_u_valid: Bool,
                input_u_bits: Array[UInt],
                input_a_valid: Bool,
                input_a_bits: Array[UInt],
                input_coeff_valid: Bool,
                input_coeff_bits: UInt
            ) = {
                dut.io.input_a.valid.poke(input_a_valid)
                dut.io.input_u.valid.poke(input_u_valid)
                for (i <- 0 to 7) {
                    dut.io.input_a.bits(i).sign.poke(input_a_bits(i)(31))
                    dut.io.input_a.bits(i).exp.poke(input_a_bits(i)(30, 23))
                    dut.io.input_a.bits(i).frac.poke(input_a_bits(i)(22, 0))
                    dut.io.input_u.bits(i).sign.poke(input_u_bits(i)(31))
                    dut.io.input_u.bits(i).exp.poke(input_u_bits(i)(30, 23))
                    dut.io.input_u.bits(i).frac.poke(input_u_bits(i)(22, 0))
                }
                dut.io.input_coeff.valid.poke(input_coeff_valid)
                dut.io.input_coeff.bits.sign.poke(input_coeff_bits(31))
                dut.io.input_coeff.bits.exp.poke(input_coeff_bits(30, 23))
                dut.io.input_coeff.bits.frac.poke(input_coeff_bits(22, 0))
                print_output()
            }

            val scalaFP_a_vec = Array(2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f,
                                    1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f,
                                    9.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f
                                    )
            val scalaFP_u_vec = Array(28.645824f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f,
                                    1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f,
                                    9.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f
                                    )
            val scalaFP_coeff = -0.9999999f
            val chiselFP_a_vec = scalaFP_a_vec.map(HelperFunctions.ScalaFloat_to_ChiselFloat)
            val chiselFP_u_vec = scalaFP_u_vec.map(HelperFunctions.ScalaFloat_to_ChiselFloat)
            val chiselFP_coeff = HelperFunctions.ScalaFloat_to_ChiselFloat(scalaFP_coeff)

            // First 8 elements
            for (x <- 0 until 3) {
                dut.reset.poke(false.B)
                dut_input(
                    true.B, chiselFP_u_vec.slice(0, 8),
                    true.B, chiselFP_a_vec.slice(0, 8),
                    false.B, 0.U
                )
                dut.clock.step(1)

                // Wait for 4 cycles
                for (d <- 0 until 4) {
                    dut_input(
                        false.B, Array.fill(8)(0.U),
                        false.B, Array.fill(8)(0.U),
                        false.B, 0.U
                    )
                    dut.clock.step(1)
                }

                // Second 8 elements
                dut_input(
                    true.B, chiselFP_u_vec.slice(8, 16),
                    true.B, chiselFP_a_vec.slice(8, 16),
                    false.B, 0.U
                )
                dut.clock.step(1)

                // Wait for 4 cycles
                for (d <- 0 until 4) {
                    dut_input(
                        false.B, Array.fill(8)(0.U),
                        false.B, Array.fill(8)(0.U),
                        false.B, 0.U
                    )
                    dut.clock.step(1)
                }

                // Third 8 elements
                dut_input(
                    true.B, chiselFP_u_vec.slice(16, 24),
                    true.B, chiselFP_a_vec.slice(16, 24),
                    false.B, 0.U
                )
                dut.clock.step(1)
            
                // Input coefficient
                dut_input(
                    false.B, Array.fill(8)(0.U),
                    false.B, Array.fill(8)(0.U),
                    true.B, chiselFP_coeff
                )
                dut.clock.step(1)
                
                dut.clock.step(1)

                // Last Cycles
                while (dut.io.output.valid.peek().litValue != 1) {
                    dut_input(
                        false.B, Array.fill(8)(0.U),
                        false.B, Array.fill(8)(0.U),
                        false.B, 0.U
                    )
                    dut.clock.step(1)
                }

                for (i <- 0 until 5) {
                    dut_input(
                        false.B, Array.fill(8)(0.U),
                        false.B, Array.fill(8)(0.U),
                        false.B, 0.U
                    )
                    dut.clock.step(1)
                }
                dut.reset.poke(true.B)
                dut.clock.step(1)
                println("Module Reset: Go to the next step")
            }
        }
    }
}