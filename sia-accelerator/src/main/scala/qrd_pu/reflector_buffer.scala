package qrd_pu

import chisel3._
import chisel3.util._
import floats.FloatType

/**
  * Reflector_Buffer: Store reflector vector and returns it whenever it is **required**
  *     - request: Return currently stored reflector vector for multiple cycles, work as a trigger
  *     - input: valid indicates valid input reflector vector
  *            : bits indicate input reflector vector
  *     - output: valid indicates valid output reflector vector
  *             : bits indicate output reflector vector -- DELAYED WITH 4 CYCLES
  * 
  * @param NUM_DOT_PE: Number of parallel multipliers at dot product tree unit
  * @param MEM_SIZE: Maximum depth of the memory
  */

class Reflector_Buffer(NUM_DOT_PE: Int, MEM_SIZE: Int) extends Module {
    val io = IO(new Bundle {
        val request = Input(Bool())
        val input = Flipped(new Valid(Vec(NUM_DOT_PE, new FloatType(8, 23))))
        val output = new Valid(Vec(NUM_DOT_PE, new FloatType(8, 23)))
    })

    val mem = SyncReadMem(MEM_SIZE, Vec(NUM_DOT_PE, UInt(32.W)))
    val is_running_reg = RegInit(false.B)   // True when returing outputs
    val write_point_reg = RegInit(0.U(log2Ceil(MEM_SIZE).W))
    val read_point_reg = RegInit(0.U(log2Ceil(MEM_SIZE).W)) // Should be reset when request signal is true
    val output_counter = RegInit(0.U(3.W))  // For delaying 4 cycles

    val valid_reg = RegInit(false.B)

    when (io.input.valid === true.B) {
        mem.write(write_point_reg, VecInit(io.input.bits.map(_.asTypeOf(UInt(32.W)))))
        write_point_reg := write_point_reg + 1.U
    }

    when (io.request === true.B) {
        is_running_reg := true.B
        read_point_reg := 0.U
        output_counter := 0.U
    }

    when (is_running_reg === true.B | io.request === true.B) {
        when (output_counter === 0.U | io.request === true.B) {
            output_counter := 4.U
            read_point_reg := read_point_reg + 1.U
            valid_reg := true.B
        } .otherwise {
            output_counter := output_counter - 1.U;
            valid_reg := false.B
        }
    } .otherwise {
        valid_reg := false.B
    }

    when (write_point_reg === read_point_reg) {
        is_running_reg := false.B
        read_point_reg := 0.U
    }

    io.output.bits := VecInit(mem.read(read_point_reg).map(_.asTypeOf(new FloatType(8, 23))))
    io.output.valid := valid_reg
}