import sys
import math
import struct

def fp2bin(num):
    return ''.join('{:0>8b}'.format(c) for c in struct.pack('!f', num))

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} [bits] [output_file]")
        exit(0)

    binary_table = []
    num_bits = int(sys.argv[1])

    with open(sys.argv[2], "w") as f:
        with open(sys.argv[2] + ".log", "w") as f_log:
            for i in range(0, 2 ** num_bits):
                binary_table.append(format(i, 'b').rjust(num_bits, '0'))
            for binary in binary_table:
                mantissa = 1
                for i in range(0, num_bits):
                    if binary[i] == '1':
                        mantissa += 1 / (2 ** (i+1))
                sqrt_num = math.sqrt(mantissa)
                sqrt_num_bin = fp2bin(sqrt_num)
                frac_bin = sqrt_num_bin[9:]
                assert(len(frac_bin) == 23)
                frac = int(frac_bin, 2)
                f.write(f"{frac}\n")
                f_log.write(f"before mantissa: {mantissa}, sqrt mantissa: {sqrt_num}, encoded mantissa: {sqrt_num_bin}, encoded frac: {frac_bin}, frac: {frac}\n")
