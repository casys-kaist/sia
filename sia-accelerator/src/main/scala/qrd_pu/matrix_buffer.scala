package qrd_pu

import chisel3._
import chisel3.util._
import floats.FloatType
import scala.collection.immutable.ArraySeq

/**
 *  Matrix_Buffer_Wrapper: Wrapper for Matrix_Buffer that supports multiple PEs and banked RAM
 *      - write_req: A vector of write requests
 *      - write_column: A vector of requested Column number
 *      - write_row: A vector of requested Row number
 *      - input: A vector of R matrix elements (length of NUM_DOT_PE)
 * 
 *      - read_req: A vector of read requests (length of PE_NUM)
 *      - read_column: A vector of requested Column number (length of PE_NUM)
 *      - read_row: A vector of requested Row number (length of PE_NUM)
 *      - output: Vectors of stored R matrix elements (length of PE_NUM * NUM_DOT_PE)
 * 
 * @param KEY_LENGTH: Key length of learned index (Row/Column size of R matrix)
 * @param PE_NUM: Number of PEs per a PU
 * @param NUM_DOT_PE: Number of parallel multipliers at dot product tree unit
 * @param MEM_SIZE: Maximum depth of the memory
 */

class Matrix_Buffer_Wrapper(KEY_LENGTH: Int, PE_NUM: Int, NUM_DOT_PE: Int, MEM_SIZE: Int) extends Module {
    val io = IO(new Bundle {
        val write_req = Input(Vec(PE_NUM, Bool()))
        val write_column = Input(Vec(PE_NUM, UInt(log2Ceil(KEY_LENGTH).W)))
        val write_row = Input(Vec(PE_NUM, UInt(log2Ceil(MEM_SIZE).W)))
        val input = Input(Vec(PE_NUM, Vec(NUM_DOT_PE, new FloatType(8, 23))))

        val read_req = Input(Vec(PE_NUM, Bool()))
        val read_column = Input(Vec(PE_NUM, UInt(log2Ceil(KEY_LENGTH).W)))
        val read_row = Input(Vec(PE_NUM, UInt(log2Ceil(MEM_SIZE).W)))
        val output = Output(Vec(PE_NUM, Vec(NUM_DOT_PE, new FloatType(8, 23))))
    })

    val matrix_buffers = VecInit(Seq.fill(KEY_LENGTH) {
        Module(new Matrix_Buffer(MEM_SIZE, NUM_DOT_PE)).io
    })

    // Init Ports
    for (i <- 0 until KEY_LENGTH) {
        matrix_buffers(i).write_req.valid := false.B
        matrix_buffers(i).write_req.bits := 0.U
        matrix_buffers(i).input.valid := false.B
        matrix_buffers(i).input.bits := VecInit(Seq.fill(NUM_DOT_PE)(0.U.asTypeOf(new FloatType(8, 23))))
        matrix_buffers(i).read_req.valid := false.B
        matrix_buffers(i).read_req.bits := 0.U
    }

    // Write Request
    for (i <- 0 until PE_NUM) {
        val current_column_num = io.write_column(i)
        when (io.write_req(i) === true.B) {
            matrix_buffers(current_column_num).write_req.valid := true.B
            matrix_buffers(current_column_num).write_req.bits := io.write_row(i)
            matrix_buffers(current_column_num).input.valid := true.B
            matrix_buffers(current_column_num).input.bits := io.input(i)
        }
    }

    // Read Request
    for (i <- 0 until PE_NUM) {
        val current_column_num = io.read_column(i)
        when (io.read_req(i) === true.B) {
            matrix_buffers(current_column_num).read_req.valid := true.B
            matrix_buffers(current_column_num).read_req.bits := io.read_row(i)
        }

        when (matrix_buffers(current_column_num).output.valid === true.B) {
            io.output(i) := matrix_buffers(current_column_num).output.bits
        } .otherwise {
            io.output(i) := VecInit(Seq.fill(NUM_DOT_PE)(0.U.asTypeOf(new FloatType(8, 23))))
        }
    }
}

/**
  * Matrix_Buffer: Store input matrix and returns it whenever it is **required**
  *                A Matrix_Buffer handles one column of the input matrix (It is bank-ed)
  *     - write_req: valid - write new input matrix element
  *                  bits - start of requested **Row** number
  *     - input: an input partial vector of input matrix (length of NUM_DOT_PE)
  * 
  *     - read_req: valid - read currently stored input matrix element
  *                 bits - start of requested **Row** number
  *     - output: an output partial vector of input matrix (length of NUM_DOT_PE)
  * 
  * @param MEM_SIZE: Maximum depth of the memory
  * @param NUM_DOT_PE: Number of parallel multipliers at dot product tree unit
  */

class Matrix_Buffer(MEM_SIZE: Int, NUM_DOT_PE: Int) extends Module {
    val io = IO(new Bundle {
        val write_req = Flipped(new Valid(UInt(log2Ceil(MEM_SIZE).W)))
        val input = Flipped(new Valid(Vec(NUM_DOT_PE, new FloatType(8, 23))))
        val read_req = Flipped(new Valid(UInt(log2Ceil(MEM_SIZE).W)))
        val output = new Valid(Vec(NUM_DOT_PE, new FloatType(8, 23)))
    })

    //val mem = SyncReadMem(MEM_SIZE, Vec(NUM_DOT_PE, new FloatType(8, 23)))
    val mem = SyncReadMem(MEM_SIZE, Vec(NUM_DOT_PE, UInt(32.W)))

    when (io.read_req.valid === true.B) {
        io.output.valid := true.B
    } .otherwise {
        io.output.valid := false.B
    }
    io.output.bits := VecInit(mem.read(io.read_req.bits).map(_.asTypeOf(new FloatType(8, 23))))

    when (io.write_req.valid === true.B) {
        assert(io.input.valid === true.B)
        mem.write(io.write_req.bits, VecInit(io.input.bits.map(_.asTypeOf(UInt(32.W)))))
    }
}
