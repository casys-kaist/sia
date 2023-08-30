package qrd_pu

import chisel3._
import chisel3.util._
import floats.FloatType
import scala.collection.immutable.ArraySeq
import helpers.HelperFunctions

/**
 * R_Matrix_Buffer_Wrapper: Wrapper for R_Matrix_Buffer that supports multiple PEs and banked RAM
 *      - write_req: A vector of write requests (length of PE_NUM)
 *      - write_column: A vector of requested Column number (length of PE_NUM)
 *      - write_row: A vector of requested Row number (length of PE_NUM)
 *      - input: A vector of R matrix elements (length of PE_NUM)
 * 
 *      - read_req: A vector of read requests (length of NUM_DOT_PE)
 *      - read_column: A vector of requested Column number (length of NUM_DOT_PE)
 *      - read_row: A vector of requested Row number (length of NUM_DOT_PE)
 *      - output: A vector of stored R matrix elements (length of NUM_DOT_PE)
 * 
 * @param KEY_LENGTH: Key length of learned index (Row/Column size of R matrix)
 * @param PE_NUM: Number of PEs per a PU
 * @param NUM_DOT_PE: Number of parallel multipliers at dot product tree unit
 */
class R_Matrix_Buffer_Wrapper(KEY_LENGTH: Int, PE_NUM: Int, NUM_DOT_PE: Int) extends Module {
    val io = IO(new Bundle {
        val write_req = Input(Vec(PE_NUM, Bool()))
        val write_column = Input(Vec(PE_NUM, UInt(log2Ceil(KEY_LENGTH).W)))
        val write_row = Input(Vec(PE_NUM, UInt(log2Ceil(KEY_LENGTH).W)))
        val input = Input(Vec(PE_NUM, new FloatType(8, 23)))

        val read_req = Input(Vec(NUM_DOT_PE, Bool()))
        val read_column = Input(Vec(NUM_DOT_PE, UInt(log2Ceil(KEY_LENGTH).W)))
        val read_row = Input(Vec(NUM_DOT_PE, UInt(log2Ceil(KEY_LENGTH).W)))
        val output = Output(Vec(NUM_DOT_PE, new FloatType(8, 23)))
    })

    val r_matrix_buffers = VecInit(Seq.fill(KEY_LENGTH)(
        Module(new R_Matrix_Buffer(KEY_LENGTH)).io
    ))

    // Init Ports
    for (i <- 0 until KEY_LENGTH) {
        r_matrix_buffers(i).write_req.valid := false.B
        r_matrix_buffers(i).write_req.bits := 0.U
        r_matrix_buffers(i).input.valid := false.B
        r_matrix_buffers(i).input.bits := 0.U.asTypeOf(new FloatType(8, 23))
        r_matrix_buffers(i).read_req.valid := false.B
        r_matrix_buffers(i).read_req.bits := 0.U
    }

    // Write Request
    for (i <- 0 until PE_NUM) {
        when (io.write_req(i) === true.B) {
            r_matrix_buffers(io.write_column(i)).write_req.valid := true.B
            r_matrix_buffers(io.write_column(i)).write_req.bits := io.write_row(i)
            r_matrix_buffers(io.write_column(i)).input.valid := true.B
            r_matrix_buffers(io.write_column(i)).input.bits := io.input(i)
        }
    }


    // Read Request
    for (i <- 0 until NUM_DOT_PE) {
        //val current_column_num = 
        when (io.read_req(i) === true.B) {
            r_matrix_buffers(io.read_column(i)).read_req.valid := true.B
            r_matrix_buffers(io.read_column(i)).read_req.bits := io.read_row(i)
        }

        when (r_matrix_buffers(io.read_column(i)).output.valid === true.B) {
            io.output(i) := r_matrix_buffers(io.read_column(i)).output.bits
        } .otherwise {
            io.output(i) := 0.U.asTypeOf(new FloatType(8, 23))
        }
    }

    //io.output := reg_output
}

/**
  * R_Matrix_Buffer: Store R matrix and returns it whenever it is **required**
  *                  The R Matrix Buffer handles one column of the R matrix (It is bank-ed)
  *     - write_req: valid - write new R matrix element
  *                  bits - requested **Row** number
  *     - input: an input element of R matrix
  * 
  *     - read_req: valid - read currently stored R matrix element
  *                 bits - requested **Row** number
  *     - output: an output element of R matrix
  * 
  * @param KEY_LENGTH: Key length of learned index (Row/Column size of R matrix)
  */

class R_Matrix_Buffer(KEY_LENGTH: Int) extends Module{
    val io = IO(new Bundle {
        val write_req = Flipped(new Valid(UInt(log2Ceil(KEY_LENGTH).W)))
        val input = Flipped(new Valid(new FloatType(8, 23)))
        val read_req = Flipped(new Valid(UInt(log2Ceil(KEY_LENGTH).W)))
        val output = new Valid(new FloatType(8, 23))
    })

    val mem = SyncReadMem(KEY_LENGTH, UInt(32.W))
    
    when (io.read_req.valid === true.B) {
        io.output.valid := true.B
    } .otherwise {
        io.output.valid := false.B
    }
    io.output.bits := mem.read(io.read_req.bits).asTypeOf(new FloatType(8, 23))
    
    when (io.write_req.valid === true.B) {
        assert(io.input.valid === true.B)
        mem.write(io.write_req.bits, io.input.bits.asTypeOf(UInt(32.W)))
    }
}
