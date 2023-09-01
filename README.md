# SIA

**SIA**: **S**tring-key **l**earned Index **A**cceleration is an algorithm-hardware co-designed string-key learned index system for efficient and scable indexing. SIA exploits a memoization technique to reduce the cost of retraining for inserted keys. This technology utilizes the algorithmic characteristics of matrix decomposition and can be applied to updatable learned indexes based on linear regression models. Also, SIA offloads model training to a dedicated FPGA accelerator, allowing model training to be accelerated and CPU resources to be focused on query processing.

This repository contains the full implementation of SIA-SW, FPGA accelerator kernel of SIA-HW written in Chisel3, and CPU-FPGA interface code for Intel Harp system. Our implementaion is based on the previous work, SIndex[^1] (https://ipads.se.sjtu.edu.cn:1312/opensource/xindex/-/tree/sindex/).

[^1]: Youyun Wang, Chuzhe Tang, Zhaoguo Wang, and Haibo Chen. 2020. SIndex: A Scalable Learned Index for String Keys. In Proceedings of the 11th ACM SIGOPS Asia-Pacific Workshop on Systems (Tsukuba, Japan) (APSys ’20).



## Directory structure

- `original`: Original version of SIndex with no modification.
- `ideal`: Idealized version of SIndex that emulates the system with no training cost.
- `sia-sw`: CPU-only SIA.
- `sia-hw`: FPGA accelerated SIA.
- `sia-accelerator/src`: FPGA accelerator code written in Chisel3.
- `sia-accelerator/intel`: FPGA-CPU interface for Intel Harp platform.



## Prerequisites

Intel MKL library is required to build this project. Follow the command below to install the library.


```sh
$ cd /tmp
$ wget https://apt.repos.intel.com/intel-gpg-keys/GPG-PUB-KEY-INTEL-SW-PRODUCTS-2019.PUB
$ apt-key add GPG-PUB-KEY-INTEL-SW-PRODUCTS-2019.PUB
$ rm GPG-PUB-KEY-INTEL-SW-PRODUCTS-2019.PUB

$ sh -c 'echo deb https://apt.repos.intel.com/mkl all main > /etc/apt/sources.list.d/intel-mkl.list'
$ apt-get update
$ apt-get install -y intel-mkl-2019.0-045
```



## Build

To build this project, you need CMake (> 3.5) and g++ (supports c++14) installed on your system. Follow the command below to build this project.

```sh
$ mkdir build
$ cd build
$ cmake ..
$ make
```



## Run

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



## Compile Chisel3 FPGA Code

You need to install [scala](https://www.scala-lang.org/download/) & [sbt](https://www.scala-sbt.org/download.html) to compile Chisel3 code into Verilog code.

```sh
$ echo "deb https://repo.scala-sbt.org/scalasbt/debian all main"
	| sudo tee /etc/apt/sources.list.d/sbt.list
$ echo "deb https://repo.scala-sbt.org/scalasbt/debian /"
	| sudo tee /etc/apt/sources.list.d/sbt_old.list
$ curl -sL "https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x2EE0EA64E40A89B84B2DF73499E82A75642AC823"
	| sudo apt-key add
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

Compiled Verilog code will be generated in the `sia-accelerator/generated` directory.



## CPU-FPGA Environment

CPU-FPGA interface code is written for Intel Harp system.

For the SW interface code, [OPAE SDK](https://opae.github.io/) is required to be build.

For the HW interface & accelerator wrapper, [Intel FPGA BBB](https://github.com/OPAE/intel-fpga-bbb) library is required to synthesize the FPGA kernel.

Quartus II v20.1 is used to synthesize the entire HW-side FPGA kernel, interface, and accelerator wrapper.