package qrd_pu

import chisel3._
import chiseltest._
import org.scalatest.flatspec.AnyFlatSpec
import helpers.HelperFunctions

class TestScalarRegister() extends AnyFlatSpec with ChiselScalatestTester {
    "ScalarRegister" should "pass" in {
        test(new Scalar_Register()) { dut =>
            val scala_first_input = -0.0013118337f
            val scala_fake_input = 3.14f
            val chisel_first_input = HelperFunctions.ScalaFloat_to_ChiselFloat(scala_first_input)
            val chisel_fake_input = HelperFunctions.ScalaFloat_to_ChiselFloat(scala_fake_input)

            def dut_input(input_valid: Bool, input_bits: UInt, reset: Bool) = {
                dut.io.input.valid.poke(input_valid)
                dut.io.input.bits.sign.poke(input_bits(31))
                dut.io.input.bits.exp.poke(input_bits(30, 23))
                dut.io.input.bits.frac.poke(input_bits(22, 0))
                dut.io.reset.poke(reset)
            }

            def dut_output(output_valid: Bool, output_bits: UInt) = {
                dut.io.output.valid.expect(output_valid)
                dut.io.output.bits.sign.expect(output_bits(31))
                dut.io.output.bits.exp.expect(output_bits(30, 23))
                dut.io.output.bits.frac.expect(output_bits(22, 0))
            }
        
            dut_input(true.B, chisel_first_input, false.B)
            dut.clock.step(1)

            dut_input(false.B, 0.U, false.B)
            dut_output(true.B, chisel_first_input)
            dut.clock.step(1)

            dut_input(true.B, chisel_fake_input, false.B)
            dut_output(true.B, chisel_first_input)
            dut.clock.step(1)

            dut_input(false.B, 0.U, true.B)
            dut_output(true.B, chisel_first_input)
            dut.clock.step(1)

            dut_input(false.B, 0.U, false.B)
            dut.io.output.valid.expect(false.B)
        }
    }
}

class TestReflectorBuffer() extends AnyFlatSpec with ChiselScalatestTester {
    val NUM_DOT_PE = 8
    val MEM_SIZE = 32
    "ReflectorBuffer" should "pass" in {
        test(new Reflector_Buffer(NUM_DOT_PE, MEM_SIZE)) { dut =>
            def dut_input(request: Bool, input_valid: Bool, input_bits: Array[UInt]) = {
                dut.io.request.poke(request)
                dut.io.input.valid.poke(input_valid)
                for (i <- 0 until NUM_DOT_PE) {
                    dut.io.input.bits(i).sign.poke(input_bits(i)(31))
                    dut.io.input.bits(i).exp.poke(input_bits(i)(30, 23))
                    dut.io.input.bits(i).frac.poke(input_bits(i)(22, 0))
                }
            }

            def print_output() = {
                val output_valid = dut.io.output.valid.peek().toString
                val output_bits = dut.io.output.bits.peek()
                print("Valid: " + output_valid + ", Bits: ")
                for (i <- 0 until NUM_DOT_PE) {
                    val output_scala = HelperFunctions.ChiselFloat_to_ScalaFloat(output_bits(i))
                    print(output_scala.toString + " ")
                }
                println("")
            }

            def dut_output(output_valid: Bool, output_bits: Array[UInt]) = {
                print_output()
                //dut.io.output.valid.expect(output_valid)
                if (output_valid == true.B) {
                    for (i <- 0 until NUM_DOT_PE) {
                        //dut.io.output.bits(i).sign.expect(output_bits(i)(31))
                        //dut.io.output.bits(i).exp.expect(output_bits(i)(30, 23))
                        //dut.io.output.bits(i).frac.expect(output_bits(i)(22, 0))
                    }
                }
            }
            
            val length = 5
            val scala_reflector = new Array[Float](NUM_DOT_PE * length)
            for (i <- 0 until NUM_DOT_PE * length) {
                scala_reflector(i) = HelperFunctions.Random_gen()
            }
            val chisel_reflector: Array[UInt] = scala_reflector.map(HelperFunctions.ScalaFloat_to_ChiselFloat)
            scala_reflector.foreach((element: Float) => print(element.toString + " "))
            println("")

            for (i <- 0 until length) {
                dut_input(false.B, true.B, chisel_reflector.slice(NUM_DOT_PE * i, NUM_DOT_PE * (i + 1)))
                dut.clock.step(1)
            }

            dut_input(false.B, false.B, Array.fill(NUM_DOT_PE)(0.U))
            dut.clock.step(1)

            for (i <- 0 until length * 5 + 1) {
                if (i == 0) {
                    dut_input(true.B, false.B, Array.fill(NUM_DOT_PE)(0.U))
                } else {
                    dut_input(false.B, false.B, Array.fill(NUM_DOT_PE)(0.U))
                }
                if (i % 5 == 1) {
                    dut_output(true.B, chisel_reflector.slice(NUM_DOT_PE * ((i - 1) / 5), NUM_DOT_PE * (((i - 1) / 5) + 1)))
                } else {
                    dut_output(false.B, Array.fill(NUM_DOT_PE)(0.U))
                }
                dut.clock.step(1)
            }

            for (i <- 0 until 12) {
                print_output()
                dut.clock.step(1)
            }

            println("================")
            dut.clock.step(1)
            for (i <- 0 until length * 5 + 1) {
                if (i == 0) {
                    dut_input(true.B, false.B, Array.fill(NUM_DOT_PE)(0.U))
                } else {
                    dut_input(false.B, false.B, Array.fill(NUM_DOT_PE)(0.U))
                }
                if (i % 5 == 1) {
                    dut_output(true.B, chisel_reflector.slice(NUM_DOT_PE * ((i - 1) / 5), NUM_DOT_PE * (((i - 1) / 5) + 1)))
                } else {
                    dut_output(false.B, Array.fill(NUM_DOT_PE)(0.U))
                }
                dut.clock.step(1)
            }
        }
    }
}

class TestRMatrixBuffer() extends AnyFlatSpec with ChiselScalatestTester {
    val KEY_LENGTH = 16
    val PE_NUM = 4
    val NUM_DOT_PE = 8
    "RMatrixBuffer" should "pass" in {
        test(new R_Matrix_Buffer_Wrapper(KEY_LENGTH, PE_NUM, NUM_DOT_PE)) { dut =>
            def dut_input(write_req: Array[Bool],
                          write_column: Array[UInt],
                          write_row: Array[UInt],
                          input: Array[UInt],
                          read_req: Array[Bool],
                          read_column: Array[UInt],
                          read_row: Array[UInt]) = {

                assert(write_req.length == PE_NUM)
                assert(write_column.length == PE_NUM)
                assert(write_row.length == PE_NUM)
                assert(input.length == PE_NUM)
                for (i <- 0 until PE_NUM) {
                    dut.io.write_req(i).poke(write_req(i))
                    dut.io.write_column(i).poke(write_column(i))
                    dut.io.write_row(i).poke(write_row(i))
                    dut.io.input(i).sign.poke(input(i)(31))
                    dut.io.input(i).exp.poke(input(i)(30, 23))
                    dut.io.input(i).frac.poke(input(i)(22, 0))
                }

                assert(read_req.length == NUM_DOT_PE)
                assert(read_column.length == NUM_DOT_PE)
                assert(read_row.length == NUM_DOT_PE)
                for (i <- 0 until NUM_DOT_PE) {
                    dut.io.read_req(i).poke(read_req(i))
                    dut.io.read_column(i).poke(read_column(i))
                    dut.io.read_row(i).poke(read_row(i))
                }
            }

            def print_output() = {
                for (i <- 0 until NUM_DOT_PE) {
                    val output_scala = HelperFunctions.ChiselFloat_to_ScalaFloat(dut.io.output(i).peek())
                    print(output_scala.toString + " ")
                }
            }

            def dut_output(output: Array[UInt]) = {
                assert(output.length == NUM_DOT_PE)
                print_output()
                for (i <- 0 until NUM_DOT_PE) {
                    dut.io.output(i).sign.expect(output(i)(31))
                    dut.io.output(i).exp.expect(output(i)(30, 23))
                    dut.io.output(i).frac.expect(output(i)(22, 0))
                }
            }

            val scala_r_matrix = Array.ofDim[Float](KEY_LENGTH, KEY_LENGTH)
            val chisel_r_matrix_valid = Array.ofDim[Bool](KEY_LENGTH, KEY_LENGTH)
            for (i <- 0 until KEY_LENGTH) { // Vertical
                for (j <- 0 until KEY_LENGTH) { // Horizontal
                    if (j >= i) {
                        chisel_r_matrix_valid(i)(j) = true.B
                        scala_r_matrix(i)(j) = HelperFunctions.Random_gen()
                    } else {
                        chisel_r_matrix_valid(i)(j) = false.B
                        scala_r_matrix(i)(j) = 0.0f
                    }
                }
            }

            val chisel_r_matrix = scala_r_matrix.map(_.map(HelperFunctions.ScalaFloat_to_ChiselFloat))

            println("R Matrix:")
            for (row <- scala_r_matrix) {
                for (element <- row) {
                    print(s"$element ")
                }
                println()
            }
            println("=========================")

            // Input Elements (Row-major)
            for (i <- 0 until KEY_LENGTH) {
                for (j <- 0 until (KEY_LENGTH / PE_NUM)){
                    dut_input(
                        write_req = chisel_r_matrix_valid(i).slice(j * PE_NUM, (j+1) * PE_NUM),
                        write_column = Array.range(j * PE_NUM, (j+1) * PE_NUM).map((e: Int) => e.U),
                        write_row = Array.fill(PE_NUM)(i.U),
                        input = chisel_r_matrix(i).slice(j * PE_NUM, (j+1) * PE_NUM),

                        read_req = Array.fill(NUM_DOT_PE)(false.B),
                        read_column = Array.fill(NUM_DOT_PE)(0.U),
                        read_row = Array.fill(NUM_DOT_PE)(0.U)
                    )
                    dut.clock.step(1)
                }
            }

            dut.clock.step(5)

            println("Stored R Matrix:")
            // Output Elements (Row-major)
            for (i <- 0 until KEY_LENGTH) {
                for (j <- 0 until (KEY_LENGTH / NUM_DOT_PE)) {
                    dut_input(
                        write_req = Array.fill(PE_NUM)(false.B),
                        write_column = Array.fill(PE_NUM)(0.U),
                        write_row = Array.fill(PE_NUM)(0.U),
                        input = Array.fill(PE_NUM)(0.U),

                        read_req = Array.fill(NUM_DOT_PE)(true.B),
                        read_column = Array.range(j * NUM_DOT_PE, (j+1) * NUM_DOT_PE).map((e: Int) => e.U),
                        read_row = Array.fill(NUM_DOT_PE)(i.U)
                    )

                    dut.clock.step(1)

                    dut_output(
                        chisel_r_matrix(i).slice(j * NUM_DOT_PE, (j+1) * NUM_DOT_PE)
                    )                    
                }
                println()
            }
            println("=========================")
        }
    }
}

class TestMatrixBuffer() extends AnyFlatSpec with ChiselScalatestTester {
    val KEY_LENGTH = 16
    val PE_NUM = 4
    val NUM_DOT_PE = 8
    val MEM_SIZE = 64

    val COLUMN_NUM = KEY_LENGTH
    val ROW_NUM = 48
    "MatrixBuffer" should "pass" in {
        test(new Matrix_Buffer_Wrapper(KEY_LENGTH, PE_NUM, NUM_DOT_PE, MEM_SIZE)) { dut =>
            def dut_input(write_req: Bool,
                          write_column: UInt,
                          write_row: UInt,
                          input: Array[UInt],
                          read_req: Array[Bool],
                          read_column: Array[UInt],
                          read_row: Array[UInt]) = {

                assert(input.length == NUM_DOT_PE)
                dut.io.write_req(0).poke(write_req)
                dut.io.write_column(0).poke(write_column)
                dut.io.write_row(0).poke(write_row)
                for (i <- 0 until NUM_DOT_PE) {
                    dut.io.input(0)(i).sign.poke(input(i)(31))
                    dut.io.input(0)(i).exp.poke(input(i)(30, 23))
                    dut.io.input(0)(i).frac.poke(input(i)(22, 0))
                }

                for (j <- 1 until PE_NUM) {
                    dut.io.write_req(j).poke(false.B)
                    dut.io.write_column(j).poke(0.U)
                    dut.io.write_row(j).poke(0.U)
                    for (i <- 0 until NUM_DOT_PE) {
                        dut.io.input(j)(i).sign.poke(0.U)
                        dut.io.input(j)(i).exp.poke(0.U)
                        dut.io.input(j)(i).frac.poke(0.U)
                    }
                }

                assert(read_req.length == PE_NUM)
                assert(read_column.length == PE_NUM)
                assert(read_row.length == PE_NUM)
                for (i <- 0 until PE_NUM) {
                    dut.io.read_req(i).poke(read_req(i))
                    dut.io.read_column(i).poke(read_column(i))
                    dut.io.read_row(i).poke(read_row(i))
                }
            }

            def dut_output(output: Array[Array[UInt]]) : Array[Array[Float]] = {
                assert(output.length == PE_NUM)
                output.map((e: Array[UInt]) => assert(e.length == NUM_DOT_PE))
                //print_output()
                val result_matrix = Array.ofDim[Float](NUM_DOT_PE, PE_NUM)
                for (i <- 0 until PE_NUM) {
                    for (j <- 0 until NUM_DOT_PE) {
                        dut.io.output(i)(j).sign.expect(output(i)(j)(31))
                        dut.io.output(i)(j).exp.expect(output(i)(j)(30, 23))
                        dut.io.output(i)(j).frac.expect(output(i)(j)(22, 0))
                        result_matrix(j)(i) = HelperFunctions.ChiselFloat_to_ScalaFloat(dut.io.output(i)(j).peek())
                    }
                }
                result_matrix
            }

            val scala_matrix = Array.ofDim[Float](ROW_NUM, COLUMN_NUM)
            for (i <- 0 until ROW_NUM) { // Vertical
                for (j <- 0 until COLUMN_NUM) { // Horizontal
                    scala_matrix(i)(j) = HelperFunctions.Random_gen()
                }
            }
            val chisel_matrix = scala_matrix.map(_.map(HelperFunctions.ScalaFloat_to_ChiselFloat))

            println("Matrix:")
            for (row <- scala_matrix) {
                for (element <- row) {
                    print(s"$element ")
                }
                println()
            }
            println("=========================")

            // Input Elements (Column-major)
            for (i <- 0 until (ROW_NUM / NUM_DOT_PE)) { // Row
                for (j <- 0 until KEY_LENGTH) { // Column
                    dut_input(
                        write_req = true.B,
                        write_column = j.U,
                        write_row = i.U,
                        input = chisel_matrix.slice(i * NUM_DOT_PE, (i + 1) * NUM_DOT_PE).map(_(j)),

                        read_req = Array.fill(PE_NUM)(false.B),
                        read_column = Array.fill(PE_NUM)(0.U),
                        read_row = Array.fill(PE_NUM)(0.U)
                    )
                    dut.clock.step(1)
                }
            }

            // Output Elements
            val result_matrix = Array.ofDim[Float](ROW_NUM, COLUMN_NUM)
            for (j <- 0 until (KEY_LENGTH / PE_NUM)) { // Column
                for (i <- 0 until (ROW_NUM / NUM_DOT_PE)) { // Row
                    dut_input(
                        write_req = false.B,
                        write_column = 0.U,
                        write_row = 0.U,
                        input = Array.fill(NUM_DOT_PE)(0.U),

                        read_req = Array.fill(PE_NUM)(true.B),
                        read_column = Array.range(j * PE_NUM, (j+1) * PE_NUM).map((e: Int) => e.U),
                        read_row = Array.fill(PE_NUM)(i.U),
                    )

                    dut.clock.step(1)

                    val read_matrix = dut_output(
                        chisel_matrix.slice(i * NUM_DOT_PE, (i+1) * NUM_DOT_PE).map(_.slice(j * PE_NUM, (j+1) * PE_NUM)).transpose
                    )

                    for (ii <- 0 until NUM_DOT_PE) {
                        for (jj <- 0 until PE_NUM) {
                            result_matrix(i * NUM_DOT_PE + ii)(j * PE_NUM + jj) = read_matrix(ii)(jj)
                        }
                    }
                }
            }

            println("Result Matrix:")
            for (row <- result_matrix) {
                for (element <- row) {
                    print(s"$element ")
                }
                println()
            }
            println("=========================")
        }
    }
}