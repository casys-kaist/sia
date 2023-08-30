# SIA

Based on SIndex (https://ipads.se.sjtu.edu.cn:1312/opensource/xindex/-/tree/sindex/)

### Directory structure

- `original`: Original version of SIndex with no modification.
- `ideal`: Idealized version of SIndex that emulates the system with no training cost.
- `sia-sw`: CPU-only SIA system.
- `sia-hw`: FPGA accelerated SIA system.
- `sia-accelerator/src`: FPGA accelerator code written in Chisel3
- `sia-accelerator/intel`: FPGA-CPU interface library for Intel Harp System

### Prerequisites

You need to install Intel MKL library to build this project.


```sh
$ cd /tmp
$ wget https://apt.repos.intel.com/intel-gpg-keys/GPG-PUB-KEY-INTEL-SW-PRODUCTS-2019.PUB
$ apt-key add GPG-PUB-KEY-INTEL-SW-PRODUCTS-2019.PUB
$ rm GPG-PUB-KEY-INTEL-SW-PRODUCTS-2019.PUB

$ sh -c 'echo deb https://apt.repos.intel.com/mkl all main > /etc/apt/sources.list.d/intel-mkl.list'
$ apt-get update
$ apt-get install -y intel-mkl-2019.0-045
```

### Build

To build this project, you need CMake (> 3.5) and g++ (supports c++14) installed on your system.

```sh
$ mkdir build
$ cd build
$ cmake ..
$ make
```

### Run

```sh
./build/sia-sw_bench \
	--fg={NUMBER OF FG THREADS} 	\
	--read={READ QUERY RATIO}		\
	--insert={INSERT QUERY RATIO}	\
	--update={UPDATE QUERY RATIO}	\
	--remove={REMOVE QUERY RATIO}	\
	--scan={RANGE QUERY RATIO}		\
	--runtime={INDEX RUNNINT TIME}	\
	--initial-size={NUMBER OF INITIALLY INSERTED KEYS} 	\
	--target-size={FINAL NUMBER OF ENTIRE KEYS} 		\
	--table-size={NUMBER OF KEYS IN THE DATASET}
```

### Compile Chisel3 FPGA Code

You need to install [scala](https://www.scala-lang.org/download/) & [sbt](https://www.scala-sbt.org/download.html) to compile Chisel3 code into Verilog code.

```sh
$ echo "deb https://repo.scala-sbt.org/scalasbt/debian all main" | sudo tee /etc/apt/sources.list.d/sbt.list
$ echo "deb https://repo.scala-sbt.org/scalasbt/debian /" | sudo tee /etc/apt/sources.list.d/sbt_old.list
$ curl -sL "https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x2EE0EA64E40A89B84B2DF73499E82A75642AC823" | sudo apt-key add
$ sudo apt-get update
$ sudo apt-get install sbt
```

```sh
$ curl -fL https://github.com/coursier/coursier/releases/latest/download/cs-x86_64-pc-linux.gz
	| gzip -d
	> cs
	&& chmod +x cs
	&& ./cs setup
```

To compile Chisel3 code, type the following:

```sh
$ cd sia-accelerator
$ sbt run
```

Compiled Verilog code will be generated in `sia-accelerator/generated` directory.

### CPU-FPGA Environment

CPU-FPGA interface code is written based on Intel Harp system.

For SW interface, [OPAE SDK](https://opae.github.io/) needs to be installed to be build.

For the HW interface & wrapper, [Intel FPGA BBB](https://github.com/OPAE/intel-fpga-bbb) library needs to be installed for synthesizing the kernel.

Quartus II v20.1 is used to synthesize the entire FPGA-side kernel, interface, and wrapper.