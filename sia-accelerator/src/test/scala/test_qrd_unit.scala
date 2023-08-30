import chisel3._
import chiseltest._
import org.scalatest.flatspec.AnyFlatSpec
import helpers.HelperFunctions
import org.apache.commons.math3.linear._

class TestQrdUnit() extends AnyFlatSpec with ChiselScalatestTester {
    val PE_NUM = 2
    val PU_NUM = 4
    val KEY_LENGTH = 32
    val NUM_DOT_PE = 8
    val MEM_SIZE = 256
    val QUEUE_SIZE = 128

    val COLUMN_NUM = KEY_LENGTH
    val ROW_NUM = 8
    "QrdUnit" should "pass" in {
        test(new QRD_Unit(PU_NUM, PE_NUM, KEY_LENGTH, NUM_DOT_PE, MEM_SIZE, QUEUE_SIZE)) { dut =>
            def print_output() = {

            }

            def print_debug() = {

            }

            def dut_input(
                write_req_valid: Bool,
                write_req_bits: UInt,
                write_column: UInt,
                write_row: UInt,
                write_input: Array[UInt],
                read_req: Bool,
                read_column: UInt,
                read_row: UInt,
                op_valid: Bool,
                op_bits: UInt,
                used_pus_valid: Bool,
                used_pus_bits: UInt,
                lengths_valid: Bool,
                lengths_bits: Array[UInt]
            ) = {
                dut.io.write_req.valid.poke(write_req_valid)
                dut.io.write_req.bits.poke(write_req_bits)
                dut.io.write_column.poke(write_column)
                dut.io.write_row.poke(write_row)
                for (i <- 0 until NUM_DOT_PE) {
                    dut.io.write_input(i).sign.poke(write_input(i)(31))
                    dut.io.write_input(i).exp.poke(write_input(i)(30, 23))
                    dut.io.write_input(i).frac.poke(write_input(i)(22, 0))
                }
                dut.io.read_req.poke(read_req)
                dut.io.read_column.poke(read_column)
                dut.io.read_row.poke(read_row)
                dut.io.op.valid.poke(op_valid)
                dut.io.op.bits.poke(op_bits)
                dut.io.used_pus.valid.poke(used_pus_valid)
                dut.io.used_pus.bits.poke(used_pus_bits)
                dut.io.lengths.valid.poke(lengths_valid)
                for (i <- 0 until PU_NUM) {
                    dut.io.lengths.bits(i).poke(lengths_bits(i))
                }
            }

            def dut_output(
                // TODO:
            ) = {
                print_debug()
            }

            // Sample Delta Matrix (Input)
            /*val scala_delta_matrix = Array.ofDim[Float](ROW_NUM, COLUMN_NUM)
            for (i <- 0 until ROW_NUM) {
                for (j <- 0 until COLUMN_NUM) {
                    scala_delta_matrix(i)(j) = HelperFunctions.Random_gen()
                }
            }
            val chisel_delta_matrix = scala_delta_matrix.map(_.map(HelperFunctions.ScalaFloat_to_ChiselFloat))

            // Sample Delta R Matrix (Output)
            val realMatrix = MatrixUtils.createRealMatrix(scala_delta_matrix.map(_.map(_.toDouble)))
            val qrDecomposition = new QRDecomposition(realMatrix)
            val rMatrix: RealMatrix = qrDecomposition.getR
            val scala_delta_r_matrix: Array[Array[Float]] = rMatrix.getData.map(_.map(_.toFloat))
            val chisel_delta_r_matrix = scala_r_matrix.map(_.map(HelperFunctions.ScalaFloat_to_ChiselFloat))

            // Sample Memoized R Matrix (Input)
            val scala_r_matrix = Array.ofDim[Float](KEY_LENGTH, KEY_LENGTH)
            for (i <- 0 until KEY_LENGTH) {
                for (j <- 0 until KEY_LENGTH) {
                    if (i <= j) scala_r_matrix(i)(j) = HelperFunctions.Random_gen()
                    else        scala_r_matrix(i)(j) = 0.0f
                }
            }
            val chisel_r_matrix = scala_r_matrix.map(_.map(HelperFunctions.ScalaFloat_to_ChiselFloat))

            // Sample Concat-ed R Matrix (Input)
            val scala_concat_r_matrix = scala_delta_matrix ++ scala_r_matrix
            val chisel_concat_r_matrix = scala_concat_r_matrix.map(_.map(HelperFunctions.ScalaFloat_to_ChiselFloat))

            // Sample Final R Matrix (Output)
            val realMatrix2 = MatrixUtils.createRealMatrix(scala_concat_r_matrix.map(_.map(_.toDouble)))
            val qrDecomposition2 = new QRDecomposition(realMatrix2)
            val rMatrix2: RealMatrix = qrDecomposition2.getR
            val scala_final_r_matrix: Array[Array[Float]] = rMatrix2.getData.map(_.map(_.toFloat))
            val chisel_final_r_matrix = scala_final_r_matrix.map(_.map(HelperFunctions.ScalaFloat_to_ChiselFloat))

            println("Input Delta Matrix:")

            println("==========================")
            println("Answer Delta R Matrix:")

            println("==========================")
            println("Input Memoized R Matrix:")

            println("==========================")
            println("Answer Final R Matrix:")

            println("==========================")

            println("Transmit input delta matrix to qrd_unit ... ")

            println("Done")

            println("Compute QR Decomposition (Trigger OP=1) ... ")

            println("Done")

            println("Redirect delta R matrix to the PU (Trigger OP=2) ... ")

            println("Done")

            println("Transmit memoized R matrix to qrd_unit ... ")
            
            println("Done")
            
            println("Compute QR Decomposition (Trigger OP=3) ... ")

            println("Done")

            println("Receive final R matrix from qrd_unit ... ")

            println("Done")*/
        }
    }
}
