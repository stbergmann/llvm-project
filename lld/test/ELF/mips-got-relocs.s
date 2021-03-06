# Check R_MIPS_GOT16 relocation calculation.

# RUN: llvm-mc -filetype=obj -triple=mips-unknown-linux %s -o %t-be.o
# RUN: ld.lld %t-be.o -o %t-be.exe
# RUN: llvm-objdump -section-headers -t %t-be.exe | FileCheck -check-prefix=EXE_SYM %s
# RUN: llvm-objdump -s -section=.got %t-be.exe | FileCheck -check-prefix=EXE_GOT_BE %s
# RUN: llvm-objdump -d %t-be.exe | FileCheck -check-prefix=EXE_DIS_BE %s
# RUN: llvm-readobj -relocations %t-be.exe | FileCheck -check-prefix=NORELOC %s
# RUN: llvm-readobj -sections %t-be.exe | FileCheck -check-prefix=SHFLAGS %s

# RUN: llvm-mc -filetype=obj -triple=mipsel-unknown-linux %s -o %t-el.o
# RUN: ld.lld %t-el.o -o %t-el.exe
# RUN: llvm-objdump -section-headers -t %t-el.exe | FileCheck -check-prefix=EXE_SYM %s
# RUN: llvm-objdump -s -section=.got %t-el.exe | FileCheck -check-prefix=EXE_GOT_EL %s
# RUN: llvm-objdump -d %t-el.exe | FileCheck -check-prefix=EXE_DIS_EL %s
# RUN: llvm-readobj -relocations %t-el.exe | FileCheck -check-prefix=NORELOC %s
# RUN: llvm-readobj -sections %t-el.exe | FileCheck -check-prefix=SHFLAGS %s

# RUN: ld.lld -shared %t-be.o -o %t-be.so
# RUN: llvm-objdump -section-headers -t %t-be.so | FileCheck -check-prefix=DSO_SYM %s
# RUN: llvm-objdump -s -section=.got %t-be.so | FileCheck -check-prefix=DSO_GOT_BE %s
# RUN: llvm-objdump -d %t-be.so | FileCheck -check-prefix=DSO_DIS_BE %s
# RUN: llvm-readobj -relocations %t-be.so | FileCheck -check-prefix=NORELOC %s
# RUN: llvm-readobj -sections %t-be.so | FileCheck -check-prefix=SHFLAGS %s

# RUN: ld.lld -shared %t-el.o -o %t-el.so
# RUN: llvm-objdump -section-headers -t %t-el.so | FileCheck -check-prefix=DSO_SYM %s
# RUN: llvm-objdump -s -section=.got %t-el.so | FileCheck -check-prefix=DSO_GOT_EL %s
# RUN: llvm-objdump -d %t-el.so | FileCheck -check-prefix=DSO_DIS_EL %s
# RUN: llvm-readobj -relocations %t-el.so | FileCheck -check-prefix=NORELOC %s
# RUN: llvm-readobj -sections %t-el.so | FileCheck -check-prefix=SHFLAGS %s

# REQUIRES: mips

  .text
  .globl  __start
__start:
  lui $2, %got(v1)

  .data
  .globl v1
  .type  v1,@object
  .size  v1,4
v1:
  .word 0

# EXE_SYM: Sections:
# EXE_SYM: .got 0000000c 0000000000030000 DATA
# EXE_SYM: SYMBOL TABLE:
# EXE_SYM: 00037ff0         .got		 00000000 .hidden _gp
#          ^-- .got + GP offset (0x7ff0)
# EXE_SYM: 00040000 g       .data		 00000004 v1


# EXE_GOT_BE: Contents of section .got:
# EXE_GOT_BE:  30000 00000000 80000000 00040000
#                    ^        ^        ^-- v1 (0x40000)
#                    |        +-- Module pointer (0x80000000)
#                    +-- Lazy resolver (0x0)

# EXE_GOT_EL: Contents of section .got:
# EXE_GOT_EL:  30000 00000000 00000080 00000400
#                    ^        ^        ^-- v1 (0x40000)
#                    |        +-- Module pointer (0x80000000)
#                    +-- Lazy resolver (0x0)

# v1GotAddr (0x3000c) - _gp (0x37ff4) = -0x7fe8 => 0x8018 = 32792
# EXE_DIS_BE:  20000:  3c 02 80 18  lui $2, 32792
# EXE_DIS_EL:  20000:  18 80 02 3c  lui $2, 32792

# DSO_SYM: Sections:
# DSO_SYM: .got 0000000c 0000000000020000 DATA
# DSO_SYM: SYMBOL TABLE:
# DSO_SYM: 00027ff0         .got		 00000000 .hidden _gp
#          ^-- .got + GP offset (0x7ff0)
# DSO_SYM: 00030000 g       .data		 00000004 v1

# DSO_GOT_BE: Contents of section .got:
# DSO_GOT_BE:  20000 00000000 80000000 00030000
#                    ^        ^        ^-- v1 (0x30000)
#                    |        +-- Module pointer (0x80000000)
#                    +-- Lazy resolver (0x0)

# DSO_GOT_EL: Contents of section .got:
# DSO_GOT_EL:  20000 00000000 00000080 00000300
#                    ^        ^        ^-- v1 (0x30000)
#                    |        +-- Module pointer (0x80000000)
#                    +-- Lazy resolver (0x0)

# v1GotAddr (0x2000c) - _gp (0x27ff4) = -0x7fe8 => 0x8018 = 32792
# DSO_DIS_BE:  10000:  3c 02 80 18  lui $2, 32792
# DSO_DIS_EL:  10000:  18 80 02 3c  lui $2, 32792

# NORELOC:      Relocations [
# NORELOC-NEXT: ]

# SHFLAGS:      Name: .got
# SHFLAGS-NEXT: Type: SHT_PROGBITS
# SHFLAGS-NEXT: Flags [ (0x10000003)
#                        ^-- SHF_MIPS_GPREL | SHF_ALLOC | SHF_WRITE
