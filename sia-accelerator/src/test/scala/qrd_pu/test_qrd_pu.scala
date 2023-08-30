package qrd_pu

import chisel3._
import chiseltest._
import org.scalatest.flatspec.AnyFlatSpec
import helpers.HelperFunctions
import org.apache.commons.math3.linear._

class TestQrdPu() extends AnyFlatSpec with ChiselScalatestTester {
    val PE_NUM = 1
    val KEY_LENGTH = 8
    val NUM_DOT_PE = 8
    val MEM_SIZE = 2
    val QUEUE_SIZE = 2

    val COLUMN_NUM = KEY_LENGTH
    val ROW_NUM = 8
    "QrdPu" should "pass" in {
        test(new QRD_PU(PE_NUM, KEY_LENGTH, NUM_DOT_PE, MEM_SIZE, QUEUE_SIZE)) { dut =>
            def dut_input(
                op: Bool,
                length_valid: Bool,
                length_bits: UInt,
                write_req: Bool,
                write_column: UInt,
                write_row: UInt,
                input: Array[UInt],
                read_req: Bool,
                read_column: UInt,
                read_row: UInt,
            ) = {
                dut.io.op.poke(op)
                dut.io.length.valid.poke(length_valid)
                dut.io.length.bits.poke(length_bits)
                dut.io.write_req.poke(write_req)
                dut.io.write_column.poke(write_column)
                dut.io.write_row.poke(write_row)
                for (i <- 0 until NUM_DOT_PE) {
                    dut.io.input(i).sign.poke(input(i)(31))
                    dut.io.input(i).exp.poke(input(i)(30, 23))
                    dut.io.input(i).frac.poke(input(i)(22, 0))
                }
                dut.io.read_req.poke(read_req)
                dut.io.read_column.poke(read_column)
                dut.io.read_row.poke(read_row)
            }

            def print_output() = {
                for (i <- 0 until NUM_DOT_PE) {
                    val output_scala = HelperFunctions.ChiselFloat_to_ScalaFloat(dut.io.output.bits(i).peek())
                    print(s"$output_scala ")
                }
            }

            def print_debug() = {
                //val state = dut.io.debug_state.peek().toString
                //val outer_column = dut.io.debug_outer_current_column.peek().toString
                //val outer_row = dut.io.debug_outer_current_row.peek().toString
                //val inner_column = dut.io.debug_inner_current_column.peek().toString
                //val inner_row = dut.io.debug_inner_current_row.peek().toString

                //println(s"State ( $state ), Outer: ( $outer_column , $outer_row ), Inner: ( $inner_column , $inner_row )")
            }

            def dut_output(
                done: Bool,
                output_valid: Bool,
                output_bits: Array[UInt],
            ) = {
                print_output()
                //dut.io.done.expect(done)
                //dut.io.output.valid.expect(output_valid)
                for (i <- 0 until NUM_DOT_PE) {
                    //dut.io.output.bits(i).sign.expect(output_bits(i)(31))
                    //dut.io.output.bits(i).exp.expect(output_bits(i)(30, 23))
                    //dut.io.output.bits(i).frac.expect(output_bits(i)(22, 0))
                }
            }

            // Sample Input Matrix
            val scala_matrix = Array.ofDim[Float](ROW_NUM, COLUMN_NUM)
            for (i <- 0 until ROW_NUM) {
                for (j <- 0 until COLUMN_NUM) {
                    scala_matrix(i)(j) = HelperFunctions.Random_gen()
                }
            }
            val chisel_matrix = scala_matrix.map(_.map(HelperFunctions.ScalaFloat_to_ChiselFloat))

            // Sample Output Matrix
            val realMatrix = MatrixUtils.createRealMatrix(scala_matrix.map(_.map(_.toDouble)))
            val qrDecomposition = new QRDecomposition(realMatrix)
            val rMatrix: RealMatrix = qrDecomposition.getR
            val scala_r_matrix: Array[Array[Float]] = rMatrix.getData.map(_.map(_.toFloat))
            val chisel_r_matrix = scala_r_matrix.map(_.map(HelperFunctions.ScalaFloat_to_ChiselFloat))

            println("Input Matrix:")
            for (row <- scala_matrix) {
                for (element <- row) {
                    print(s"$element ")
                }
                println()
            }
            println("==========================")
            println("Answer R Matrix:")
            for (row <- scala_r_matrix) {
                for (element <- row) {
                    print(s"$element ")
                }
                println()
            }
            println("==========================")

            // Transmit input matrix to qrd_pu (Column major)
            println("Transmit input matrix to qrd_pu ... ")
            for (i <- 0 until (ROW_NUM / NUM_DOT_PE)) {
                for (j <- 0 until KEY_LENGTH) {
                    print(s"$i, $j\r")
                    dut_input(
                        op = false.B,
                        length_valid = false.B,
                        length_bits = 0.U,

                        write_req = true.B,
                        write_column = j.U,
                        write_row = i.U,
                        input = chisel_matrix.slice(i * NUM_DOT_PE, (i + 1) * NUM_DOT_PE).map(_(j)),

                        read_req = false.B,
                        read_column = 0.U,
                        read_row = 0.U,
                    )
                    dut.clock.step(1)
                }
            }
            println("Done")
            
            // Trigger qrd_pu
            print("Trigger qrd_pu ... ")
            dut_input(
                op = true.B,
                length_valid = true.B,
                length_bits = (ROW_NUM / NUM_DOT_PE).U,

                write_req = false.B,
                write_column = 0.U,
                write_row = 0.U,
                input = Array.fill(NUM_DOT_PE)(0.U),

                read_req = false.B,
                read_column = 0.U,
                read_row = 0.U
            )
            dut.clock.step(1)
            println("Done")

            println("Waiting to be done ... ")
            while(dut.io.done.peek().litValue != 1) {
                dut_input(
                    op = false.B,
                    length_valid = false.B,
                    length_bits = 0.U,

                    write_req = false.B,
                    write_column = 0.U,
                    write_row = 0.U,
                    input = Array.fill(NUM_DOT_PE)(0.U),

                    read_req = false.B,
                    read_column = 0.U,
                    read_row = 0.U
                )
                dut.clock.step(1)
                print_debug()
            }
            println("Done")

            // Receive output R matrix from qrd_pu (Row major)
            println("Receive output R matrix from qrd_pu ... ")
            for (i <- 0 until KEY_LENGTH) {
                for (j <- 0 until (KEY_LENGTH / NUM_DOT_PE)) {
                    dut_input(
                        op = false.B,
                        length_valid = false.B,
                        length_bits = 0.U,

                        write_req = false.B,
                        write_column = 0.U,
                        write_row = i.U,
                        input = Array.fill(NUM_DOT_PE)(0.U),

                        read_req = true.B,
                        read_column = (j * NUM_DOT_PE).U,
                        read_row = i.U,
                    )

                    dut.clock.step(1)

                    dut_output(
                        false.B,
                        true.B,
                        chisel_r_matrix(i).slice(j * NUM_DOT_PE, (j+1) * NUM_DOT_PE)
                    )
                }
                println()
            }
        }
    }
}