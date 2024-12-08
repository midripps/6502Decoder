#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "em_65816.h"
#include "defs.h"
#include "memory.h"

// ====================================================================
// Type Defs
// ====================================================================

typedef enum {
   INDX,
   INDY,
   IND,
   IDL,
   IDLY,
   ZPX,
   ZPY,
   ZP,
   // All direct page modes are <= ZP
   ABS,
   ABSX,
   ABSY,
   IND16,
   IND1X,
   SR,
   ISY,
   ABL,
   ALX,
   IAL,
   BRL,
   BM,
   IMP,
   IMPA,
   BRA,
   IMM
} AddrMode ;

typedef enum {
   READOP,
   WRITEOP,
   RMWOP,
   BRANCHOP,
   OTHER
} OpType;

typedef struct {
   int len;
   const char *fmt;
} AddrModeType;

typedef int operand_t;

typedef int ea_t;

typedef struct {
   const char *mnemonic;
   int undocumented;
   AddrMode mode;
   int cycles;
   int newop;
   OpType optype;
   int (*emulate)(operand_t, ea_t);
   int len;
   int m_extra;
   int x_extra;
   const char *fmt;
} InstrType;


// ====================================================================
// Static variables
// ====================================================================

#define OFFSET_B    2
#define OFFSET_A    4
#define OFFSET_X    9
#define OFFSET_Y   16
#define OFFSET_SH  24
#define OFFSET_SL  26
#define OFFSET_N   31
#define OFFSET_V   35
#define OFFSET_MS  39
#define OFFSET_XS  43
#define OFFSET_D   47
#define OFFSET_I   51
#define OFFSET_Z   55
#define OFFSET_C   59
#define OFFSET_E   63
#define OFFSET_PB  68
#define OFFSET_DB  74
#define OFFSET_DP  80
#define OFFSET_END 84

static const char default_state[] = "A=???? X=???? Y=???? SP=???? N=? V=? M=? X=? D=? I=? Z=? C=? E=? PB=?? DB=?? DP=????";

static InstrType *instr_table;

AddrModeType addr_mode_table[] = {
   {2,    "%1$s (%2$02X,X)"},          // INDX
   {2,    "%1$s (%2$02X),Y"},          // INDY
   {2,    "%1$s (%2$02X)"},            // IND
   {2,    "%1$s [%2$02X]"},            // IDL
   {2,    "%1$s [%2$02X],Y"},          // IDLY
   {2,    "%1$s %2$02X,X"},            // ZPX
   {2,    "%1$s %2$02X,Y"},            // ZPY
   {2,    "%1$s %2$02X"},              // ZP
   {3,    "%1$s %3$02X%2$02X"},        // ABS
   {3,    "%1$s %3$02X%2$02X,X"},      // ABSX
   {3,    "%1$s %3$02X%2$02X,Y"},      // ABSY
   {3,    "%1$s (%3$02X%2$02X)"},      // IND1
   {3,    "%1$s (%3$02X%2$02X,X)"},    // IND1X
   {2,    "%1$s %2$02X,S"},            // SR
   {2,    "%1$s (%2$02X,S),Y"},        // ISY
   {4,    "%1$s %4$02X%3$02X%2$02X"},  // ABL
   {4,    "%1$s %4$02X%3$02X%2$02X,X"},// ABLX
   {3,    "%1$s [%3$02X%2$02X]"},      // IAL
   {3,    "%1$s %2$s"},                // BRL
   {3,    "%1$s %3$02X,%2$02X"},       // BM
   {1,    "%1$s"},                     // IMP
   {1,    "%1$s A"},                   // IMPA
   {2,    "%1$s %2$s"},                // BRA
   {2,    "%1$s #%2$02X"},             // IMM
};

static const char *fmt_imm16 = "%1$s #%3$02X%2$02X";

// 6502 registers: -1 means unknown
static int A = -1;
static int X = -1;
static int Y = -1;

static int SH = -1;
static int SL = -1;

static int PC = -1;

// 65C816 additional registers: -1 means unknown
static int B  = -1; // Accumulator bits 15..8
static int DP = -1; // 16-bit Direct Page Register (default to zero, otherwise ZP addressing is broken)
static int DB = -1; // 8-bit Data Bank Register
static int PB = -1; // 8-bit Program Bank Register

// 6502 flags: -1 means unknown
static int N = -1;
static int V = -1;
static int D = -1;
static int I = -1;
static int Z = -1;
static int C = -1;

// 65C816 additional flags: -1 means unknown
static int MS = -1; // Accumulator and Memeory Size Flag
static int XS = -1; // Index Register Size Flag
static int E =  -1; // Emulation Mode Flag, updated by XCE

static char *x1_ops[] = {
   "CPX",
   "CPY",
   "LDX",
   "LDY",
   "PHX",
   "PHY",
   "PLX",
   "PLY",
   "STX",
   "STY",
   NULL
};

static char *m1_ops[] = {
   "ADC",
   "AND",
   "BIT",
   "CMP",
   "EOR",
   "LDA",
   "ORA",
   "PHA",
   "PLA",
   "SBC",
   "STA",
   "STZ",
   NULL
};

static char *m2_ops[] = {
   "ASL",
   "DEC",
   "INC",
   "LSR",
   "ROL",
   "ROR",
   "TSB",
   "TRB",
   NULL
};

// ====================================================================
// Forward declarations
// ====================================================================

static InstrType instr_table_65c816[INSTR_SET_SIZE];

static void emulation_mode_on();
static void emulation_mode_off();
static int op_STA(operand_t operand, ea_t ea);
static int op_STX(operand_t operand, ea_t ea);
static int op_STY(operand_t operand, ea_t ea);

// ====================================================================
// Helper Methods
// ====================================================================

static int compare_FLAGS(int operand) {
   if (N >= 0) {
      if (N != ((operand >> 7) & 1)) {
         return 1;
      }
   }
   if (V >= 0) {
      if (V != ((operand >> 6) & 1)) {
         return 1;
      }
   }
   if (E == 0 && MS >= 0) {
      if (MS != ((operand >> 5) & 1)) {
         return 1;
      }
   }
   if (E == 0 && XS >= 0) {
      if (XS != ((operand >> 4) & 1)) {
         return 1;
      }
   }
   if (D >= 0) {
      if (D != ((operand >> 3) & 1)) {
         return 1;
      }
   }
   if (I >= 0) {
      if (I != ((operand >> 2) & 1)) {
         return 1;
      }
   }
   if (Z >= 0) {
      if (Z != ((operand >> 1) & 1)) {
         return 1;
      }
   }
   if (C >= 0) {
      if (C != ((operand >> 0) & 1)) {
         return 1;
      }
   }
   return 0;
}

static void check_FLAGS(int operand) {
   failflag |= compare_FLAGS(operand);
}

static void x_flag_updated() {
   if (XS) {
      if (X >= 0) {
         X &= 0x00ff;
      }
      if (Y >= 0) {
         Y &= 0x00ff;
      }
   }
}

static void set_FLAGS(int operand) {
   N = (operand >> 7) & 1;
   V = (operand >> 6) & 1;
   if (E == 0) {
      MS = (operand >> 5) & 1;
      XS = (operand >> 4) & 1;
   } else {
      MS = 1;
      XS = 1;
   }
   x_flag_updated();
   D = (operand >> 3) & 1;
   I = (operand >> 2) & 1;
   Z = (operand >> 1) & 1;
   C = (operand >> 0) & 1;
}

static void set_NZ_unknown() {
   N = -1;
   Z = -1;
}

static void set_NZC_unknown() {
   N = -1;
   Z = -1;
   C = -1;
}

static void set_NVZC_unknown() {
   N = -1;
   V = -1;
   Z = -1;
   C = -1;
}

static void set_NZ8(int value) {
   N = (value >> 7) & 1;
   Z = (value & 0xff) == 0;
}

static void set_NZ16(int value) {
   N = (value >> 15) & 1;
   Z = (value & 0xffff) == 0;
}

static void set_NZ_unknown_width(int value) {
   // Don't know which bit is the sign bit
   int s15 = (value >> 15) & 1;
   int s7 = (value >> 7) & 1;
   if (s7 == s15) {
      // both choices of sign bit are the same
      N = s7;
   } else {
      // possible sign bits differ, so N must become undefined
      N = -1;
   }
   // Don't know how many bits to check for any ones
   if ((value & 0xff00) == 0) {
      // no high bits set, so base Z on the low bits
      Z = (value & 0xff) == 0;
   } else {
      // some high bits set, so Z must become undefined
      Z = -1;
   }
}

static void set_NZ_XS(int value) {
   if (XS < 0) {
      set_NZ_unknown_width(value);
   } else if (XS == 0) {
      set_NZ16(value);
   } else {
      set_NZ8(value);
   }
}

static void set_NZ_MS(int value) {
   if (MS < 0) {
      set_NZ_unknown_width(value);
   } else if (MS == 0) {
      set_NZ16(value);
   } else {
      set_NZ8(value);
   }
}

static void set_NZ_AB(int A, int B) {
   if (MS > 0) {
      // 8-bit
      if (A >= 0) {
         set_NZ8(A);
      } else {
         set_NZ_unknown();
      }
   } else if (MS == 0) {
      // 16-bit
      if (A >= 0 && B >= 0) {
         set_NZ16((B << 8) + A);
      } else {
         // TODO: the behaviour when A is known and B is unknown could be improved
         set_NZ_unknown();
      }
   } else {
      // width unknown
      if (A >= 0 && B >= 0) {
         set_NZ_unknown_width((B << 8) + A);
      } else {
         set_NZ_unknown();
      }
   }
}

// Helper routine to handle incrementing the stack pointer
static void incSP() {
   // Increment the low byte of SP
   if (SL >= 0) {
      SL = (SL + 1) & 0xff;
   }
   // Increment the high byte of SP, in certain cases
   if (E == 1) {
      // In emulation mode, force SH to 1
      SH = 1;
   } else if (E == 0) {
      // In native mode, increment SH if SL has wrapped to 0
      if (SH >= 0) {
         if (SL < 0) {
            SH = -1;
         } else if (SL == 0) {
            SH = (SH + 1) & 0xff;
         }
      }
   } else {
      SH = -1;
   }
}

// Helper routine to handle decrementing the stack pointer
static void decSP() {
   // Decrement the low byte of SP
   if (SL >= 0) {
      SL = (SL - 1) & 0xff;
   }
   // Decrement the high byte of SP, in certain cases
   if (E == 1) {
      // In emulation mode, force SH to 1
      SH = 1;
   } else if (E == 0) {
      // In native mode, increment SH if SL has wrapped to 0
      if (SH >= 0) {
         if (SL < 0) {
            SH = -1;
         } else if (SL == 0xff) {
            SH = (SH - 1) & 0xff;
         }
      }
   } else {
      SH = -1;
   }
}

// pop one byte off the stack - used by "old" instructions
static void pop8(int value) {
   // Increment/wrap the stack pointer
   incSP();
   // Handle the memory access
   if (SL >= 0 && SH >= 0) {
      memory_read(value & 0xff, (SH << 8) + SL, MEM_STACK);
   }
}

// pop one byte off the stack - used by "new" instructions
static void pop8new(int value) {
   // Handle the memory access
   if (SL >= 0 && SH >= 0) {
      memory_read(value & 0xff, ((SH << 8) + SL + 1) & 0xffff, MEM_STACK);
   }
   // Increment/wrap the stack pointer
   incSP();
}

// pop two bytes off the stack - used by "old" instructions
static void pop16(int value) {
   pop8(value);
   pop8(value >> 8);
}


// pop two bytes off the stack - used by "new" instructions (e.g. PLD)
static void pop16new(int value) {
   // Handle the memory access
   if (SL >= 0 && SH >= 0) {
      memory_read( value       & 0xff, ((SH << 8) + SL + 1) & 0xffff, MEM_STACK);
      memory_read((value >> 8) & 0xff, ((SH << 8) + SL + 2) & 0xffff, MEM_STACK);
   }
   // Increment/wrap the stack pointer
   incSP();
   incSP();
}

// pop three bytes off the stack - used by "new" instructions (e.g.RTL)
static void pop24new(int value) {
   // Handle the memory access
   if (SL >= 0 && SH >= 0) {
      memory_read( value        & 0xff, ((SH << 8) + SL + 1) & 0xffff, MEM_STACK); // PCL
      memory_read((value >>  8) & 0xff, ((SH << 8) + SL + 2) & 0xffff, MEM_STACK); // PCH
      memory_read((value >> 16) & 0xff, ((SH << 8) + SL + 3) & 0xffff, MEM_STACK); // PBR
   }
   // Increment/wrap the stack pointer
   incSP();
   incSP();
   incSP();
}

// push one byte onto the stack - used by "old" instructions and "new" instructions
static void push8(int value) {
   // Handle the memory access
   if (SL >= 0 && SH >= 0) {
      memory_write(value & 0xff, (SH << 8) + SL, MEM_STACK);
   }
   // Decrement/wrap the stack pointer
   decSP();
}

// push two byte onto the stack - used by "old" instructions
static void push16(int value) {
   push8(value >> 8);
   push8(value);
}

// push two byte onto the stack - used by "new" instructions
static void push16new(int value) {
   // Handle the memory access
   if (SL >= 0 && SH >= 0) {
      memory_write((value >> 8) & 0xff, (SH << 8) + SL, MEM_STACK);
      memory_write(value & 0xff, ((SH << 8) + SL - 1) & 0xffff, MEM_STACK);
   }
   // Decrement/wrap the stack pointer
   decSP();
   decSP();
}

static void popXS(int value) {
   if (XS < 0) {
      SL = -1;
      SH = -1;
   } else if (XS == 0) {
      pop16(value); // TODO: should be new?
   } else {
      pop8(value);
   }
}

static void popMS(int value) {
   if (MS < 0) {
      SL = -1;
      SH = -1;
   } else if (MS == 0) {
      pop16(value); // TODO: should be new?
   } else {
      pop8(value);
   }
}

static void pushXS(int value) {
   if (XS < 0) {
      SL = -1;
      SH = -1;
   } else if (XS == 0) {
      push16(value);
   } else {
      push8(value);
   }
}

static void pushMS(int value) {
   if (MS < 0) {
      SL = -1;
      SH = -1;
   } else if (MS == 0) {
      push16(value);
   } else {
      push8(value);
   }
}

static void interrupt(sample_t *sample_q, int num_cycles, instruction_t *instruction, int pc_offset) {
   int i;
   int pb;
   if (num_cycles == 7) {
      // We must be in emulation mode
      emulation_mode_on();
      i = 2;
      pb = PB;
   } else {
      // We must be in native mode
      emulation_mode_off();
      i = 3;
      pb = sample_q[2].data;
   }
   // Parse the bus cycles
   // E=0 <opcode> <op1> <write pbr> <write pch> <write pcl> <write p> <read rst> <read rsth>
   // E=1 <opcode> <op1>             <write pch> <write pcl> <write p> <read rst> <read rsth>
   int pc     = (sample_q[i].data << 8) + sample_q[i + 1].data;
   int flags  = sample_q[i + 2].data;
   int vector = (sample_q[i + 4].data << 8) + sample_q[i + 3].data;
   // Update the address of the interruted instruction
   if (pb >= 0) {
      instruction->pb = pb;
   }
   instruction->pc = (pc - pc_offset) & 0xffff;
   // Stack the PB/PC/FLags (for memory modelling)
   if (E == 0) {
      push8(pb);
   }
   push16(pc);
   push8(flags);
   // Validate the flags
   check_FLAGS(flags);
   // And make them consistent
   set_FLAGS(flags);
   // Setup expected state for the ISR
   I = 1;
   D = 0;
   PB = 0x00;
   PC = vector;
}

static int get_8bit_cycles(sample_t *sample_q) {
   int opcode = sample_q[0].data;
   int op1    = sample_q[1].data;
   int op2    = sample_q[2].data;
   InstrType *instr = &instr_table[opcode];
   int cycle_count = instr->cycles;

   // One cycle penalty if DP is not page aligned
   int dpextra = (instr->mode <= ZP && DP >= 0 && (DP & 0xff)) ? 1 : 0;

   // Account for extra cycle in a page crossing in (indirect), Y (not stores)
   // <opcode> <op1> [ <dpextra> ] <addrlo> <addrhi> [ <page crossing>] <operand> [ <extra cycle in dec mode> ]
   if ((instr->mode == INDY) && (instr->optype != WRITEOP) && Y >= 0) {
      int base = (sample_q[3 + dpextra].data << 8) + sample_q[2 + dpextra].data;
      if ((base & 0x1ff00) != ((base + Y) & 0x1ff00)) {
         cycle_count++;
      }
   }

   // Account for extra cycle in a page crossing in absolute indexed (not stores or rmw) in emulated mode
   if (((instr->mode == ABSX) || (instr->mode == ABSY)) && (instr->optype == READOP)) {
      int index = (instr->mode == ABSX) ? X : Y;
      if (index >= 0) {
         int base = op1 + (op2 << 8);
         if ((base & 0x1ff00) != ((base + index) & 0x1ff00)) {
            cycle_count++;
         }
      }
   }

   return cycle_count + dpextra;
}

static int get_num_cycles(sample_t *sample_q, int intr_seen) {
   int opcode = sample_q[0].data;
   int op1    = sample_q[1].data;
   int op2    = sample_q[2].data;
   InstrType *instr = &instr_table[opcode];
   int cycle_count = instr->cycles;

   // Interrupt, BRK, COP
   if (intr_seen || opcode == 0x00 || opcode == 0x02) {
      return (E == 0) ? 8 : 7;
   }

   // E MS    Correction:
   // ?  ?    ?
   // ?  0    ?
   // 0  ?    ?
   // 0  0    1
   // ?  1    0
   // 0  1    0
   // 1  ?    0
   // 1  0    0
   // 1  1    0

   if (instr->m_extra) {
      if (E == 0 && MS == 0) {
         cycle_count += instr->m_extra;
      } else if (!(E > 0 || MS > 0)) {
         return -1;
      }
   }

   if (instr->x_extra) {
      if (E == 0 && XS == 0) {
         cycle_count += instr->x_extra;
      } else if (!(E > 0 || XS > 0)) {
         return -1;
      }
   }


   // One cycle penalty if DP is not page aligned
   int dpextra = (instr->mode <= ZP && DP >= 0 && (DP & 0xff)) ? 1 : 0;

   // RTI takes one extra cycle in native mode
   if (opcode == 0x40) {
      if (E == 0) {
         cycle_count++;
      } else if (E < 0) {
         return -1;
      }
   }

   // Account for extra cycle in a page crossing in (indirect), Y (not stores)
   // <opcode> <op1> [ <dpextra> ] <addrlo> <addrhi> [ <page crossing>] <operand> [ <extra cycle in dec mode> ]
   if ((instr->mode == INDY) && (instr->optype != WRITEOP) && Y >= 0) {
      int base = (sample_q[3 + dpextra].data << 8) + sample_q[2 + dpextra].data;
      // TODO: take account of page crossing with 16-bit Y
      if ((base & 0x1ff00) != ((base + Y) & 0x1ff00)) {
         cycle_count++;
      }
   }

   // Account for extra cycle in a page crossing in absolute indexed (not stores or rmw)
   if (((instr->mode == ABSX) || (instr->mode == ABSY)) && (instr->optype == READOP)) {
      int correction = -1;
      int index = (instr->mode == ABSX) ? X : Y;
      if (index >= 0) {
         int base = op1 + (op2 << 8);
         if ((base & 0x1ff00) != ((base + index) & 0x1ff00)) {
            correction = 1;
         } else {
            correction = 0;
         }
      }
      // XS  C
      //  ?  ?    ?
      //  ?  0    ?
      //  ?  1    1
      //  0  ?    1
      //  0  0    1
      //  0  1    1
      //  1  ?    ?
      //  1  0    0
      //  1  1    1
      if (XS == 0 || correction == 1) {
         cycle_count++;
      } else if (XS < 0 || correction < 0) {
         return -1;
      }
   }

   // Account for extra cycles in a branch
   if (((opcode & 0x1f) == 0x10) || (opcode == 0x80)) {
      // Default to backards branches taken, forward not taken
      // int taken = ((int8_t)op1) < 0;
      int taken = -1;
      switch (opcode) {
      case 0x10: // BPL
         if (N >= 0) {
            taken = !N;
         }
         break;
      case 0x30: // BMI
         if (N >= 0) {
            taken = N;
         }
         break;
      case 0x50: // BVC
         if (V >= 0) {
            taken = !V;
         }
         break;
      case 0x70: // BVS
         if (V >= 0) {
            taken = V;
         }
         break;
      case 0x80: // BRA
         taken = 1;
         cycle_count--; // instr table contains 3 for cycle count
         break;
      case 0x90: // BCC
         if (C >= 0) {
            taken = !C;
         }
         break;
      case 0xB0: // BCS
         if (C >= 0) {
            taken = C;
         }
         break;
      case 0xD0: // BNE
         if (Z >= 0) {
            taken = !Z;
         }
         break;
      case 0xF0: // BEQ
         if (Z >= 0) {
            taken = Z;
         }
         break;
      }
      if (taken < 0) {
         return -1;
      } else if (taken) {
         // A taken branch is 3 cycles, not 2
         cycle_count++;
         // In emulation node, a taken branch that crosses a page boundary is 4 cycle
         int page_cross = -1;
         if (E > 0 && PC >= 0) {
            int target =  (PC + 2) + ((int8_t)(op1));
            if ((target & 0xFF00) != ((PC + 2) & 0xff00)) {
               page_cross = 1;
            } else {
               page_cross = 0;
            }
         } else if (E == 0) {
            page_cross = 0;
         }
         if (page_cross < 0) {
            return -1;
         } else {
            cycle_count += page_cross;
         }
      }
   }

   return cycle_count + dpextra;
}


static int count_cycles_without_sync(sample_t *sample_q, int intr_seen) {
   //printf("VPA/VDA must be connected in 65816 mode\n");
   //exit(1);
   int num_cycles = get_num_cycles(sample_q, intr_seen);
   if (num_cycles >= 0) {
      return num_cycles;
   }
   printf ("cycle prediction unknown\n");
   return 1;
}

static int count_cycles_with_sync(sample_t *sample_q, int intr_seen) {
   if (sample_q[0].type == OPCODE) {
      for (int i = 1; i < DEPTH; i++) {
         if (sample_q[i].type == LAST) {
            return 0;
         }
         if (sample_q[i].type == OPCODE) {
            // Validate the num_cycles passed in
            int expected = get_num_cycles(sample_q, intr_seen);
            if (expected >= 0) {
               if (i != expected) {
                  printf ("opcode %02x: cycle prediction fail: expected %d actual %d\n", sample_q[0].data, expected, i);
               }
            }
            return i;
         }
      }
   }
   return 1;
}

// A set of actions to take if emulation mode enabled
static void emulation_mode_on() {
   if (E == 0) {
      failflag = 1;
   }
   MS = 1;
   XS = 1;
   x_flag_updated();
   SH = 0x01;
   E = 1;
}

// A set of actions to take if emulation mode enabled
static void emulation_mode_off() {
   if (E == 1) {
      failflag = 1;
   }
   E = 0;
}

static void check_and_set_ms(int val) {
   if (MS >= 0 &&  MS != val) {
      failflag = 1;
   }
   MS = val;
   // Evidence of MS = 0 implies E = 0
   if (MS == 0) {
      emulation_mode_off();
   }
}

static void check_and_set_xs(int val) {
   if (XS >= 0 &&  XS != val) {
      failflag = 1;
   }
   XS = val;
   x_flag_updated();
   // Evidence of XS = 0 implies E = 0
   if (XS == 0) {
      emulation_mode_off();
   }
}

// Helper to return the variable size accumulator
static int get_accumulator() {
   if (MS > 0 && A >= 0) {
      // 8-bit mode
      return A;
   } else if (MS == 0 && A >= 0 && B >= 0) {
      // 16-bit mode
      return (B << 8) + A;
   } else {
      // unknown width
      return -1;
   }
}

// ====================================================================
// Public Methods
// ====================================================================

static void em_65816_init(arguments_t *args) {
   switch (args->cpu_type) {
   case CPU_65C816:
      instr_table = instr_table_65c816;
      break;
   default:
      printf("em_65816_init called with unsupported cpu_type (%d)\n", args->cpu_type);
      exit(1);
   }
   if (args->e_flag >= 0) {
      E  = args->e_flag & 1;
      if (E) {
         emulation_mode_on();
      } else {
         emulation_mode_off();
      }
   }
   if (args->sp_reg >= 0) {
      SL = args->sp_reg & 0xff;
      SH = (args->sp_reg >> 8) & 0xff;
   }
   if (args->pb_reg >= 0) {
      PB = args->pb_reg & 0xff;
   }
   if (args->db_reg >= 0) {
      DB = args->db_reg & 0xff;
   }
   if (args->dp_reg >= 0) {
      DP = args->dp_reg & 0xffff;
   }
   if (args->ms_flag >= 0) {
      MS = args->ms_flag & 1;
   }
   if (args->xs_flag >= 0) {
      XS = args->xs_flag & 1;
   }
   InstrType *instr = instr_table;
   for (int i = 0; i < INSTR_SET_SIZE; i++) {
      // Compute the extra cycles for the 816 when M=0 and/or X=0
      instr->m_extra = 0;
      instr->x_extra = 0;
      if (instr->mode != IMPA) {
         // add 1 cycle if m=0: ADC, AND, BIT, CMP, EOR, LDA, ORA, PHA, PLA, SBC, STA, STZ
         for (int j = 0; m1_ops[j]; j++) {
            if (!strcmp(instr->mnemonic, m1_ops[j])) {
               instr->m_extra++;
               break;
            }
         }
         // add 2 cycles if m=0 (NOT the implied ones): ASL, DEC, INC, LSR, ROL, ROR, TRB, TSB
         for (int j = 0; m2_ops[j]; j++) {
            if (!strcmp(instr->mnemonic, m2_ops[j])) {
               instr->m_extra += 2;
               break;
            }
         }
         // add 1 cycle if x=0: CPX, CPY, LDX, LDY, STX, STY, PLX, PLY, PHX, PHY
         for (int j = 0; x1_ops[j]; j++) {
            if (!strcmp(instr->mnemonic, x1_ops[j])) {
               instr->x_extra++;
               break;
            }
         }
      }
      // Copy the length and format from the address mode, for efficiency
      instr->len = addr_mode_table[instr->mode].len;
      instr->fmt = addr_mode_table[instr->mode].fmt;
      //printf("%02x %d %d %d\n", i, instr->m_extra, instr->x_extra, instr->len);
      instr++;
   }
}

static int em_65816_match_interrupt(sample_t *sample_q, int num_samples) {
   // Check we have enough valid samples
   if (num_samples < 7) {
      return 0;
   }
   // Check the cycle has the right structure
   for (int i = 1; i < 7; i++) {
      if (sample_q[i].type == OPCODE) {
         return 0;
      }
   }
   // In emulation mode an interupt will write PCH, PCL, PSW in bus cycles 2,3,4
   // In native mode an interupt will write PBR, PCH, PCL, PSW in bus cycles 2,3,4,5
   //
   // TODO: the heuristic only works in emulation mode
   if (sample_q[0].rnw >= 0) {
      // If we have the RNW pin connected, then just look for these three writes in succession
      // Currently can't detect a BRK or COP being interrupted
      if (sample_q[0].data == 0x00 || sample_q[0].data == 0x02) {
         return 0;
      }
      if (sample_q[2].rnw == 0 && sample_q[3].rnw == 0 && sample_q[4].rnw == 0) {
         return 1;
      }
   } else {
      // If not, then we use a heuristic, based on what we expect to see on the data
      // bus in cycles 2, 3 and 4, i.e. PCH, PCL, PSW
      if (sample_q[2].data == ((PC >> 8) & 0xff) && sample_q[3].data == (PC & 0xff)) {
         // Now test unused flag is 1, B is 0
         if ((sample_q[4].data & 0x30) == 0x20) {
            // Finally test all other known flags match
            if (!compare_FLAGS(sample_q[4].data)) {
               // Matched PSW = NV-BDIZC
               return 1;
            }
         }
      }
   }
   return 0;
}

static int em_65816_count_cycles(sample_t *sample_q, int intr_seen) {
   if (sample_q[0].type == UNKNOWN) {
      return count_cycles_without_sync(sample_q, intr_seen);
   } else {
      return count_cycles_with_sync(sample_q, intr_seen);
   }
}

static void em_65816_reset(sample_t *sample_q, int num_cycles, instruction_t *instruction) {
   instruction->pc = -1;
   A = -1;
   X = -1;
   Y = -1;
   SH = -1;
   SL = -1;
   N = -1;
   V = -1;
   D = -1;
   Z = -1;
   C = -1;
   I = 1;
   D = 0;
   // Extra 816 regs
   B = -1;
   DP = 0;
   PB = 0;
   // Extra 816 flags
   E = 1;
   emulation_mode_on();
   // Program Counter
   PC = (sample_q[num_cycles - 1].data << 8) + sample_q[num_cycles - 2].data;
}

static void em_65816_interrupt(sample_t *sample_q, int num_cycles, instruction_t *instruction) {
   interrupt(sample_q, num_cycles, instruction, 0);
}

static void em_65816_emulate(sample_t *sample_q, int num_cycles, instruction_t *instruction) {

   // Unpack the instruction bytes
   int opcode = sample_q[0].data;

   // Update the E flag if this e pin is being sampled
   int new_E = sample_q[0].e;
   if (new_E >= 0 && E != new_E) {
      if (E >= 0) {
         printf("correcting e flag\n");
         failflag |= 1;
      }
      E = new_E;
      if (E) {
         emulation_mode_on();
      } else {
         emulation_mode_off();
      }
   }

   // lookup the entry for the instruction
   InstrType *instr = &instr_table[opcode];

   // Infer MS from instruction length
   if (MS < 0 && instr->m_extra) {
      int cycles = get_8bit_cycles(sample_q);
      check_and_set_ms((num_cycles > cycles) ? 0 : 1);
   }

   // Infer XS from instruction length
   if (XS < 0 && instr->x_extra) {
      int cycles = get_8bit_cycles(sample_q);
      check_and_set_xs((num_cycles > cycles) ? 0 : 1);
   }

   // Work out outcount, taking account of 8/16 bit immediates
   int opcount = 0;
   if (instr->mode == IMM) {
      if ((instr->m_extra && MS == 0) || (instr->x_extra && XS == 0)) {
         opcount = 1;
      }
   }
   opcount += instr->len - 1;

   int op1 = (opcount < 1) ? 0 : sample_q[1].data;

   // Special case JSR (IND16, X)
   int op2 = (opcount < 2) ? 0 : (opcode == 0xFC) ? sample_q[4].data : sample_q[2].data;

   int op3 = (opcount < 3) ? 0 : sample_q[(opcode == 0x22) ? 5 : 3].data;

   // Memory Modelling: Instruction fetches
   if (PB >= 0 && PC >= 0) {
      int pc = (PB << 16) + PC;
      memory_read(opcode, pc++, MEM_FETCH);
      if (opcount >= 1) {
         memory_read(op1, pc++, MEM_INSTR);
      }
      if (opcount >= 2) {
         memory_read(op2, pc++, MEM_INSTR);
      }
      if (opcount >= 3) {
         memory_read(op3, pc++, MEM_INSTR);
      }
   }

   // Save the instruction state
   instruction->opcode  = opcode;
   instruction->op1     = op1;
   instruction->op2     = op2;
   instruction->op3     = op3;
   instruction->opcount = opcount;


   // Fill in the current PB/PC value
   if (opcode == 0x00 || opcode == 0x02) {
      // BRK or COP - handle in the same way as an interrupt
      // Now just pass BRK onto the interrupt handler
      interrupt(sample_q, num_cycles, instruction, 2);
      // And we are done
      return;
   } else if (opcode == 0x20) {
      // JSR: <opcode> <op1> <op2> <read dummy> <write pch> <write pcl>
      instruction->pc = (((sample_q[4].data << 8) + sample_q[5].data) - 2) & 0xffff;
      instruction->pb = PB;
   } else if (opcode == 0x22) {
      // JSL: <opcode> <op1> <op2> <write pbr> <read dummy> <op3> <write pch> <write pcl>
      instruction->pc = (((sample_q[6].data << 8) + sample_q[7].data) - 3) & 0xffff;
      instruction->pb = sample_q[3].data;
   } else {
      instruction->pc = PC;
      instruction->pb = PB;
   }

   // Take account for optional extra cycle for direct register low (DL) not equal 0.
   int dpextra = (instr->mode <= ZP && DP >= 0 && (DP & 0xff)) ? 1 : 0;

   // DP page wrapping only happens:
   // - in Emulation Mode (E=1), and
   // - if DPL == 00, and
   // - only for old instructions
   int wrap = E && !(DP & 0xff) && !(instr->newop);

   // Memory Modelling: Pointer indirection
   switch (instr->mode) {
   case INDY:
      // <opcode> <op1> [ <dpextra> ] <addrlo> <addrhi> [ <page crossing>] <operand>
      if (DP >= 0) {
         if (wrap) {
            memory_read(sample_q[2 + dpextra].data, (DP & 0xFF00) +                op1, MEM_POINTER);
            memory_read(sample_q[3 + dpextra].data, (DP & 0xFF00) + ((op1 + 1) & 0xff), MEM_POINTER);
         } else {
            memory_read(sample_q[2 + dpextra].data, (DP + op1    ) & 0xffff, MEM_POINTER);
            memory_read(sample_q[3 + dpextra].data, (DP + op1 + 1) & 0xffff, MEM_POINTER);
         }
      }
      break;
   case INDX:

      // In emulation mode when the low byte of D is nonzero, the
      // (direct,X) addressing mode behaves strangely:
      //
      // The low byte of the indirect address is read from
      // `direct_addr+X+D` without page wrapping (as expected).
      //
      // The high byte is read from `direct_addr+X+D+1`, but the +1 is
      // done *with* wrapping within the page.
      //
      // For example: Emulation=1, D=$11A, X=$EE, and the instruction
      // is `lda ($F7,X)`. Here $F7 + $11A + $EE = $2FF. The low byte
      // of the address is read from $2FF and the high byte from
      // $200. This behavior only applies to this addressing mode, and
      // not to other indirect modes.

      // <opcode> <op1> [ <dpextra> ] <dummy> <addrlo> <addrhi> <operand>
      if (DP >= 0 && X >= 0) {
         if (wrap) {
            memory_read(sample_q[3 + dpextra].data, (DP & 0xFF00) + ((op1 + X    ) & 0xff), MEM_POINTER);
            memory_read(sample_q[4 + dpextra].data, (DP & 0xFF00) + ((op1 + X + 1) & 0xff), MEM_POINTER);
         } else {
            memory_read(sample_q[3 + dpextra].data, (DP + op1 + X) & 0xffff, MEM_POINTER);
            if (E) {
               // This one is very strange, see above cooment
               memory_read(sample_q[4 + dpextra].data, ((DP + op1 + X) & 0xff00) + ((DP + op1 + X + 1) & 0xff), MEM_POINTER);
            } else {
               memory_read(sample_q[4 + dpextra].data, (DP + op1 + X + 1) & 0xffff, MEM_POINTER);
            }
         }
      }
      break;
   case IND:
      // <opcode> <op1>  [ <dpextra> ] <addrlo> <addrhi> <operand>
      if (DP >= 0) {
         if (wrap) {
            memory_read(sample_q[2 + dpextra].data, (DP & 0xFF00) + op1               , MEM_POINTER);
            memory_read(sample_q[3 + dpextra].data, (DP & 0xFF00) + ((op1 + 1) & 0xff), MEM_POINTER);
         } else {
            memory_read(sample_q[2 + dpextra].data, (DP + op1    ) & 0xffff, MEM_POINTER);
            memory_read(sample_q[3 + dpextra].data, (DP + op1 + 1) & 0xffff, MEM_POINTER);
         }
      }
      break;
   case ISY:
      // e.g. LDA (08, S),Y
      // <opcode> <op1> <internal> <addrlo> <addrhi> <internal> <operand>
      if (SL >= 0 && SH >= 0) {
         memory_read(sample_q[3].data, ((SH << 8) + SL + op1    ) & 0xffff, MEM_POINTER);
         memory_read(sample_q[4].data, ((SH << 8) + SL + op1 + 1) & 0xffff, MEM_POINTER);
      }
      break;
   case IDL:
      // e.g. LDA [80]
      // <opcode> <op1> [ <dpextra> ] <addrlo> <addrhi> <bank> <operand>
      if (DP >= 0) {
         memory_read(sample_q[2 + dpextra].data, (DP + op1    ) & 0xffff, MEM_POINTER);
         memory_read(sample_q[3 + dpextra].data, (DP + op1 + 1) & 0xffff, MEM_POINTER);
         memory_read(sample_q[4 + dpextra].data, (DP + op1 + 2) & 0xffff, MEM_POINTER);
      }
      break;
   case IDLY:
      // e.g. LDA [80],Y
      // <opcode> <op1> [ <dpextra> ] <addrlo> <addrhi> <bank> <operand>
      if (DP >= 0) {
         memory_read(sample_q[2 + dpextra].data, (DP + op1    ) & 0xffff, MEM_POINTER);
         memory_read(sample_q[3 + dpextra].data, (DP + op1 + 1) & 0xffff, MEM_POINTER);
         memory_read(sample_q[4 + dpextra].data, (DP + op1 + 2) & 0xffff, MEM_POINTER);
      }
      break;
   case IAL:
      // e.g. JMP [$1234] (this is the only one)
      // <opcode> <op1> <op2> <addrlo> <addrhi> <bank>
      memory_read(sample_q[3].data,  (op2 << 8) + op1              , MEM_POINTER);
      memory_read(sample_q[4].data, ((op2 << 8) + op1 + 1) & 0xffff, MEM_POINTER);
      memory_read(sample_q[5].data, ((op2 << 8) + op1 + 2) & 0xffff, MEM_POINTER);
      break;
   case IND16:
      // e.g. JMP (1234)
      // <opcode> <op1> <op2> <addrlo> <addrhi>
      memory_read(sample_q[3].data,  (op2 << 8) + op1              , MEM_POINTER);
      memory_read(sample_q[4].data, ((op2 << 8) + op1 + 1) & 0xffff, MEM_POINTER);
      break;
   case IND1X:
      // JMP: <opcode=6C> <op1> <op2> <read new pcl> <read new pch>
      // JSR: <opcode=FC> <op1> <write pch> <write pcl> <op2> <internal> <read new pcl> <read new pch>
      if (PB >= 0 && X >= 0) {
         memory_read(sample_q[num_cycles - 2].data, (PB << 16) + (((op2 << 8) + op1 + X    ) & 0xffff), MEM_POINTER);
         memory_read(sample_q[num_cycles - 1].data, (PB << 16) + (((op2 << 8) + op1 + X + 1) & 0xffff), MEM_POINTER);
      }
      break;
   default:
      break;
   }

   uint32_t operand;
   if (instr->optype == RMWOP) {
      // 2/12/2020: on Beeb816 the <read old> cycle seems hard to sample
      // reliably with the FX2, so lets use the <dummy> instead.
      // E=1 - Dummy is a write of the same data
      // <opcode> <op1> <op2> <read old> <write old> <write new>
      // E=0 - Dummy is an internal cycle (with VPA/VDA=00)
      // MS == 1:       <opcode> <op1> <op2> <read lo> <read hi> <dummy> <write hi> <write lo>
      // MS == 0:       <opcode> <op1> <op2> <read> <dummy> <write>
      if (E == 1) {
         operand = sample_q[num_cycles - 2].data;
      } else if (MS == 0) {
         // 16-bit mode
         operand = (sample_q[num_cycles - 4].data << 8) + sample_q[num_cycles - 5].data;
      } else {
         // 8-bit mode
         operand = sample_q[num_cycles - 3].data;
      }
   } else if (instr->optype == BRANCHOP) {
      // the operand is true if branch taken
      operand = (num_cycles != 2);
   } else if (opcode == 0x20) {
      // JSR abs: the operand is the data pushed to the stack (PCH, PCL)
      // <opcode> <op1> <op2> <read dummy> <write pch> <write pcl>
      operand = (sample_q[4].data << 8) + sample_q[5].data;
   } else if (opcode == 0xfc) {
      // JSR (IND, X): the operand is the data pushed to the stack (PCH, PCL)
      // <opcode> <op1> <write pch> <write pcl> <op2> <internal> <read new pcl> <read new pch>
      operand = (sample_q[2].data << 8) + sample_q[3].data;
   } else if (opcode == 0x22) {
      // JSL: the operand is the data pushed to the stack (PCB, PCH, PCL)
      // <opcode> <op1> <op2> <write pbr> <read dummy> <op3> <write pch> <write pcl>
      operand = (sample_q[3].data << 16) + (sample_q[6].data << 8) + sample_q[7].data;
   } else if (opcode == 0x40) {
      // RTI: the operand is the data pulled from the stack (P, PCL, PCH)
      // E=0: <opcode> <op1> <read dummy> <read p> <read pcl> <read pch> <read pbr>
      // E=1: <opcode> <op1> <read dummy> <read p> <read pcl> <read pch>
      operand = (sample_q[5].data << 16) +  (sample_q[4].data << 8) + sample_q[3].data;
      if (num_cycles == 6) {
         emulation_mode_on();
      } else {
         emulation_mode_off();
         operand |= (sample_q[6].data << 24);
      }
   } else if (opcode == 0x60) {
      // RTS: the operand is the data pulled from the stack (PCL, PCH)
      // <opcode> <op1> <read dummy> <read pcl> <read pch> <read dummy>
      operand = (sample_q[4].data << 8) + sample_q[3].data;
   } else if (opcode == 0x6B) {
      // RTL: the operand is the data pulled from the stack (PCL, PCH, PBR)
      // <opcode> <op1> <read dummy> <read pcl> <read pch> <read pbr>
      operand = (sample_q[5].data << 16) + (sample_q[4].data << 8) + sample_q[3].data;
   } else if (instr->mode == BM) {
      // Block Move
      operand = sample_q[3].data;
   } else if (instr->mode == IMM) {
      // Immediate addressing mode: the operand is the 2nd byte of the instruction
      operand = (op2 << 8) + op1;
   } else {
      // default to using the last bus cycle(s) as the operand
      // special case PHD (0B) / PLD (2B) / PEI (D4) as these are always 16-bit
      if ((instr->m_extra && (MS == 0)) || (instr->x_extra && (XS == 0)) || opcode == 0x0B || opcode == 0x2B || opcode == 0xD4)  {
         // 16-bit operation
         if (opcode == 0x48 || opcode == 0x5A || opcode == 0xDA || opcode == 0x0B || opcode == 0xD4) {
            // PHA/PHX/PHY/PHD push high byte followed by low byte
            operand = sample_q[num_cycles - 1].data + (sample_q[num_cycles - 2].data << 8);
         } else {
            // all other 16-bit ops are low byte then high byer
            operand = sample_q[num_cycles - 2].data + (sample_q[num_cycles - 1].data << 8);
         }
      } else {
         // 8-bit operation
         operand = sample_q[num_cycles - 1].data;
      }
   }

   // Operand 2 is the value written back in a store or read-modify-write
   // See RMW comment above for bus cycles
   operand_t operand2 = operand;
   if (instr->optype == RMWOP) {
      if (E == 0 && ((instr->m_extra && (MS == 0)) || (instr->x_extra && (XS == 0)))) {
         // 16-bit - byte ordering is high then low
         operand2 = (sample_q[num_cycles - 2].data << 8) + sample_q[num_cycles - 1].data;
      } else {
         // 8-bit
         operand2 = sample_q[num_cycles - 1].data;
      }
   } else if (instr->optype == WRITEOP) {
      if (E == 0 && ((instr->m_extra && (MS == 0)) || (instr->x_extra && (XS == 0)))) {
         // 16-bit - byte ordering is low then high
         operand2 = (sample_q[num_cycles - 1].data << 8) + sample_q[num_cycles - 2].data;
      } else {
         operand2 = sample_q[num_cycles - 1].data;
      }
   }

   // For instructions that read or write memory, we need to work out the effective address
   // Note: not needed for stack operations, as S is used directly
   int ea = -1;
   int index;
   switch (instr->mode) {
   case ZP:
      if (DP >= 0) {
         ea = (DP + op1) & 0xffff; // always bank 0
      }
      break;
   case ZPX:
   case ZPY:
      index = instr->mode == ZPX ? X : Y;
      if (index >= 0 && DP >= 0) {
         if (wrap) {
            ea = (DP & 0xff00) + ((op1 + index) & 0xff);
         } else {
            ea = (DP + op1 + index) & 0xffff; // always bank 0
         }
      }
      break;
   case INDY:
      // <opcode> <op1> [ <dpextra> ] <addrlo> <addrhi> [ <page crossing>] <operand>
      index = Y;
      if (index >= 0 && DB >= 0) {
         ea = (sample_q[3 + dpextra].data << 8) + sample_q[2 + dpextra].data;
         ea = ((DB << 16) + ea + index) & 0xffffff;
      }
      break;
   case INDX:
      // <opcode> <op1> [ <dpextra> ] <dummy> <addrlo> <addrhi> <operand>
      if (DB >= 0) {
         ea = (DB << 16) + (sample_q[4 + dpextra].data << 8) + sample_q[3 + dpextra].data;
      }
      break;
   case IND:
      // <opcode> <op1>  [ <dpextra> ] <addrlo> <addrhi> <operand>
      if (DB >= 0) {
         ea = (DB << 16) + (sample_q[3 + dpextra].data << 8) + sample_q[2 + dpextra].data;
      }
      break;
   case ABS:
      if (DB >= 0) {
         ea = (DB << 16) + (op2 << 8) + op1;
      }
      break;
   case ABSX:
   case ABSY:
      index = instr->mode == ABSX ? X : Y;
      if (index >= 0 && DB >= 0) {
         ea = ((DB << 16) + (op2 << 8) + op1 + index) & 0xffffff;
      }
      break;
   case BRA:
      if (PC > 0) {
         ea = (PC + ((int8_t)(op1)) + 2) & 0xffff;
      }
      break;
   case SR:
      // e.g. LDA 08,S
      if (SL >= 0 && SH >= 0) {
         ea = ((SH << 8) + SL + op1) & 0xffff;
      }
      break;
   case ISY:
      // e.g. LDA (08, S),Y
      // <opcode> <op1> <internal> <addrlo> <addrhi> <internal> <operand>
      index = Y;
      if (index >= 0 && DB >= 0) {
         ea = (DB << 16) + (sample_q[4].data << 8) + sample_q[3].data;
         ea = (ea + index) & 0xffffff;
      }
      break;
   case IDL:
      // e.g. LDA [80]
      // <opcode> <op1> [ <dpextra> ] <addrlo> <addrhi> <bank> <operand>
      ea = (sample_q[4 + dpextra].data << 16) + (sample_q[3 + dpextra].data << 8) + sample_q[2 + dpextra].data;
      break;
   case IDLY:
      // e.g. LDA [80],Y
      // <opcode> <op1> [ <dpextra> ] <addrlo> <addrhi> <bank> <operand>
      index = Y;
      if (index >= 0) {
         ea = (sample_q[4 + dpextra].data << 16) + (sample_q[3 + dpextra].data << 8) + sample_q[2 + dpextra].data;
         ea = (ea + index) & 0xffffff;
      }
      break;
   case ABL:
      // e.g. LDA EE0080
      ea = (op3 << 16) + (op2 << 8) + op1;
      break;
   case ALX:
      // e.g. LDA EE0080,X
      if (X >= 0) {
         ea = ((op3 << 16) + (op2 << 8) + op1 + X) & 0xffffff;
      }
      break;
   case IAL:
      // e.g. JMP [$1234] (this is the only one)
      // <opcode> <op1> <op2> <addrlo> <addrhi> <bank>
      ea = (sample_q[5].data << 16) + (sample_q[4].data << 8) + sample_q[3].data;
      break;
   case BRL:
      // e.g. PER 1234 or BRL 1234
      if (PC > 0) {
         ea = (PC + ((int16_t)((op2 << 8) + op1)) + 3) & 0xffff;
      }
      break;
   case BM:
      // e.g. MVN 0, 2
      ea = (op2 << 8) + op1;
      break;
   default:
      // covers IMM, IMP, IMPA, IND16, IND1X
      break;
   }

   if (instr->emulate) {

      // Is direct page access, as this wraps within bank 0
      int isDP = instr->mode == ZP || instr->mode == ZPX || instr->mode == ZPY;

      // Determine memory access size
      int size = instr->x_extra ? XS : instr->m_extra ? MS : 1;

      // Model memory reads
      if (ea >= 0 && (instr->optype == READOP || instr->optype == RMWOP)) {
         int oplo = (operand & 0xff);
         int ophi = ((operand >> 8) & 0xff);
         if (size == 0) {
            memory_read(oplo,  ea    , MEM_DATA);
            if (isDP) {
               memory_read(ophi,  (ea + 1) & 0xffff, MEM_DATA);
            } else {
               memory_read(ophi,  ea + 1, MEM_DATA);
            }
         } else if (size > 0) {
            memory_read(oplo, ea, MEM_DATA);
         }
      }

      // Execute the instruction specific function
      // (This returns -1 if the result is unknown or invalid)
      int result = instr->emulate(operand, ea);

      if (instr->optype == WRITEOP || instr->optype == RMWOP) {

         // STA STX STY STZ
         // INC DEX ASL LSR ROL ROR
         // TSB TRB

         // Check result of instruction against bye
         if (result >= 0 && result != operand2) {
            failflag |= 1;
         }

         // Model memory writes based on result seen on bus
         if (ea >= 0) {
            memory_write(operand2 & 0xff,  ea, MEM_DATA);
            if (size == 0) {
               if (isDP) {
                  memory_write((operand2 >> 8) & 0xff, (ea + 1) & 0xffff, MEM_DATA);
               } else {
                  memory_write((operand2 >> 8) & 0xff, ea + 1, MEM_DATA);
               }
            }
         }
      }
   }

   // Look for control flow changes and update the PC
   if (opcode == 0x40) {
      // E=0: <opcode> <op1> <read dummy> <read p> <read pcl> <read pch> <read pbr>
      // E=1: <opcode> <op1> <read dummy> <read p> <read pcl> <read pch>
      PC = sample_q[4].data | (sample_q[5].data << 8);
      if (E == 0) {
         PB = sample_q[6].data;
      }
   } else if (opcode == 0x6c || opcode == 0x7c || opcode == 0xfc ) {
      // JMP (ind), JMP (ind, X), JSR (ind, X)
      PC = (sample_q[num_cycles - 1].data << 8) | sample_q[num_cycles - 2].data;
   } else if (opcode == 0x20 || opcode == 0x4c) {
      // JSR abs, JMP abs
      // Don't use ea here as it includes PB which may be unknown
      PC = (op2 << 8) + op1;
   } else if (opcode == 0x22 || opcode == 0x5c || opcode == 0xdc) {
      // JSL long, JML long
      PB = (ea >> 16) & 0xff;
      PC = (ea & 0xffff);
   } else if (PC < 0) {
      // PC value is not known yet, everything below this point is relative
      PC = -1;
   } else if (opcode == 0x80 || opcode == 0x82) {
      // BRA / BRL
      PC = ea;
   } else if ((opcode & 0x1f) == 0x10 && num_cycles != 2) {
      // BXX: if taken
      PC = ea;
   } else {
      // Otherwise, increment pc by length of instuction
      PC = (PC + opcount + 1) & 0xffff;
   }
}

static int em_65816_disassemble(char *buffer, instruction_t *instruction) {

   int numchars;
   int offset;
   char target[16];

   // Unpack the instruction bytes
   int opcode  = instruction->opcode;
   int op1     = instruction->op1;
   int op2     = instruction->op2;
   int op3     = instruction->op3;
   int pc      = instruction->pc;
   int opcount = instruction->opcount;
   // lookup the entry for the instruction
   InstrType *instr = &instr_table[opcode];

   const char *mnemonic = instr->mnemonic;
   const char *fmt = instr->fmt;
   switch (instr->mode) {
   case IMP:
   case IMPA:
      numchars = sprintf(buffer, fmt, mnemonic);
      break;
   case BRA:
      // Calculate branch target using op1 for normal branches
      offset = (int8_t) op1;
      if (pc < 0) {
         if (offset < 0) {
            sprintf(target, "pc-%d", -offset);
         } else {
            sprintf(target,"pc+%d", offset);
         }
      } else {
         sprintf(target, "%04X", (pc + 2 + offset) & 0xffff);
      }
      numchars = sprintf(buffer, fmt, mnemonic, target);
      break;
   case BRL:
      // Calculate branch target using op1 for normal branches
      offset = (int16_t) ((op2 << 8) + op1);
      if (pc < 0) {
         if (offset < 0) {
            sprintf(target, "pc-%d", -offset);
         } else {
            sprintf(target,"pc+%d", offset);
         }
      } else {
         sprintf(target, "%04X", (pc + 3 + offset) & 0xffff);
      }
      numchars = sprintf(buffer, fmt, mnemonic, target);
      break;
   case IMM:
      if (opcount == 2) {
         numchars = sprintf(buffer, fmt_imm16, mnemonic, op1, op2);
      } else {
         numchars = sprintf(buffer, fmt, mnemonic, op1);
      }
      break;
   case ZP:
   case ZPX:
   case ZPY:
   case INDX:
   case INDY:
   case IND:
   case SR:
   case ISY:
   case IDL:
   case IDLY:
      numchars = sprintf(buffer, fmt, mnemonic, op1);
      break;
   case ABS:
   case ABSX:
   case ABSY:
   case IND16:
   case IND1X:
   case IAL:
   case BM:
      numchars = sprintf(buffer, fmt, mnemonic, op1, op2);
      break;
   case ABL:
   case ALX:
      numchars = sprintf(buffer, fmt, mnemonic, op1, op2, op3);
      break;
   default:
      numchars = 0;
   }

   return numchars;
}

static int em_65816_get_PC() {
   return PC;
}

static int em_65816_get_PB() {
   return PB;
}

static int em_65816_read_memory(int address) {
   return memory_read_raw(address);
}

static char *em_65816_get_state(char *buffer) {
   strcpy(buffer, default_state);
   if (B >= 0) {
      write_hex2(buffer + OFFSET_B, B);
   }
   if (A >= 0) {
      write_hex2(buffer + OFFSET_A, A);
   }
   if (X >= 0) {
      write_hex4(buffer + OFFSET_X, X);
   }
   if (Y >= 0) {
      write_hex4(buffer + OFFSET_Y, Y);
   }
   if (SH >= 0) {
      write_hex2(buffer + OFFSET_SH, SH);
   }
   if (SL >= 0) {
      write_hex2(buffer + OFFSET_SL, SL);
   }
   if (N >= 0) {
      buffer[OFFSET_N] = '0' + N;
   }
   if (V >= 0) {
      buffer[OFFSET_V] = '0' + V;
   }
   if (MS >= 0) {
      buffer[OFFSET_MS] = '0' + MS;
   }
   if (XS >= 0) {
      buffer[OFFSET_XS] = '0' + XS;
   }
   if (D >= 0) {
      buffer[OFFSET_D] = '0' + D;
   }
   if (I >= 0) {
      buffer[OFFSET_I] = '0' + I;
   }
   if (Z >= 0) {
      buffer[OFFSET_Z] = '0' + Z;
   }
   if (C >= 0) {
      buffer[OFFSET_C] = '0' + C;
   }
   if (E >= 0) {
      buffer[OFFSET_E] = '0' + E;
   }
   if (PB >= 0) {
      write_hex2(buffer + OFFSET_PB, PB);
   }
   if (DB >= 0) {
      write_hex2(buffer + OFFSET_DB, DB);
   }
   if (DP >= 0) {
      write_hex4(buffer + OFFSET_DP, DP);
   }
   return buffer + OFFSET_END;
}

static int em_65816_get_and_clear_fail() {
   int ret = failflag;
   failflag = 0;
   return ret;
}

cpu_emulator_t em_65816 = {
   .init = em_65816_init,
   .match_interrupt = em_65816_match_interrupt,
   .count_cycles = em_65816_count_cycles,
   .reset = em_65816_reset,
   .interrupt = em_65816_interrupt,
   .emulate = em_65816_emulate,
   .disassemble = em_65816_disassemble,
   .get_PC = em_65816_get_PC,
   .get_PB = em_65816_get_PB,
   .read_memory = em_65816_read_memory,
   .get_state = em_65816_get_state,
   .get_and_clear_fail = em_65816_get_and_clear_fail,
};

// ====================================================================
// 65816 specific instructions
// ====================================================================

// Push Effective Absolute Address
static int op_PEA(operand_t operand, ea_t ea) {
   // always pushes a 16-bit value
   push16new(ea);
   return -1;
}

// Push Effective Relative Address
static int op_PER(operand_t operand, ea_t ea) {
   // always pushes a 16-bit value
   push16new(ea);
   return -1;
}

// Push Effective Indirect Address
static int op_PEI(operand_t operand, ea_t ea) {
   // always pushes a 16-bit value
   push16new(operand);
   return -1;
}

// Push Data Bank Register
static int op_PHB(operand_t operand, ea_t ea) {
   push8(operand); // stack wrapping on push8 is same for old and new instructions
   if (DB >= 0) {
      if (operand != DB) {
         failflag = 1;
      }
   }
   DB = operand;
   return -1;
}

// Push Program Bank Register
static int op_PHK(operand_t operand, ea_t ea) {
   push8(operand); // stack wrapping on push8 is same for old and new instructions
   if (PB >= 0) {
      if (operand != PB) {
         failflag = 1;
      }
   }
   PB = operand;
   return -1;
}

// Push Direct Page Register
static int op_PHD(operand_t operand, ea_t ea) {
   push16new(operand);
   if (DP >= 0) {
      if (operand != DP) {
         failflag = 1;
      }
   }
   DP = operand;
   return -1;
}

// Pull Data Bank Register
static int op_PLB(operand_t operand, ea_t ea) {
   DB = operand;
   set_NZ8(DB);
   pop8new(operand);
   return -1;
}

// Pull Direct Page Register
static int op_PLD(operand_t operand, ea_t ea) {
   DP = operand;
   set_NZ16(DP);
   pop16new(operand);
   return -1;
}

static int op_MV(int data, int sba, int dba, int dir) {
   // operand is the data byte (from the bus read)
   // ea = (op2 << 8) + op1 == (srcbank << 8) + dstbank;
   if (X >= 0) {
      memory_read(data, (sba << 16) + X, MEM_DATA);
   }
   if (Y >= 0) {
      memory_write(data, (dba << 16) + Y, MEM_DATA);
   }
   if (A >= 0 && B >= 0) {
      int C = (((B << 8) | A) - 1) & 0xffff;
      A = C & 0xff;
      B = (C >> 8) & 0xff;
      if (XS > 0) {
         // 8-bit mode
         if (X >= 0) {
            X = (X + dir) & 0xff;
         }
         if (Y >= 0) {
            Y = (Y + dir) & 0xff;
         }
      } else if (XS == 0) {
         // 16-bit mode
         if (X >= 0) {
            X = (X + dir) & 0xffff;
         }
         if (Y >= 0) {
            Y = (Y + dir) & 0xffff;
         }
      } else {
         // mode undefined
         // TODO: we could be less pessimistic and only go to undefined
         // when a page boundary is crossed
         X = -1;
         Y = -1;
      }
      if (PC >= 0 && C != 0xffff) {
         PC -= 3;
      }
   } else {
      A = -1;
      B = -1;
      X = -1;
      Y = -1;
      PC = -1;
   }
   // Set the Data Bank to the destination bank
   DB = dba;
   return -1;
}

// Block Move (Decrementing)
static int op_MVP(operand_t operand, ea_t ea) {
   return op_MV(operand, (ea >> 8) & 0xff, ea & 0xff, -1);
}

// Block Move (Incrementing)
static int op_MVN(operand_t operand, ea_t ea) {
   return op_MV(operand, (ea >> 8) & 0xff, ea & 0xff, 1);
}

// Transfer Transfer C accumulator to Direct Page register
static int op_TCD(operand_t operand, ea_t ea) {
   // Always a 16-bit transfer
   if (B >= 0 && A >= 0) {
      DP = (B << 8) + A;
      set_NZ16(DP);
   } else {
      DP = -1;
      set_NZ_unknown();
   }
   return -1;
}

// Transfer Transfer C accumulator to Stack pointer
static int op_TCS(operand_t operand, ea_t ea) {
   SH = B;
   SL = A;
   // Force SH to be 1 in emulation mode
   if (E == 1) {
      SH = 1;
   } else if (E < 0 && SH != 1) {
      SH = -1;
   }
   return -1;
}

// Transfer Transfer Direct Page register to C accumulator
static int op_TDC(operand_t operand, ea_t ea) {
   // Always a 16-bit transfer
   if (DP >= 0) {
      A = DP & 0xff;
      B = (DP >> 8) & 0xff;
      set_NZ16(DP);
   } else {
      A = -1;
      B = -1;
      set_NZ_unknown();
   }
   return -1;
}

// Transfer Transfer Stack pointer to C accumulator
static int op_TSC(operand_t operand, ea_t ea) {
   // Always a 16-bit transfer
   A = SL;
   B = SH;
   if (B >= 0 && A >= 0) {
      set_NZ16((B << 8) + A);
   } else {
      set_NZ_unknown();
   }
   return -1;
}

static int op_TXY(operand_t operand, ea_t ea) {
   // Variable size transfer controlled by XS
   if (X >= 0) {
      Y = X;
      set_NZ_XS(Y);
   } else {
      Y = -1;
      set_NZ_unknown();
   }
   return -1;
}

static int op_TYX(operand_t operand, ea_t ea) {
   // Variable size transfer controlled by XS
   if (Y >= 0) {
      X = Y;
      set_NZ_XS(X);
   } else {
      X = -1;
      set_NZ_unknown();
   }
   return -1;
}

// Exchange A and B
static int op_XBA(operand_t operand, ea_t ea) {
   int tmp = A;
   A = B;
   B = tmp;
   if (A >= 0) {
      // Always based on the 8-bit result of A
      set_NZ8(A);
   } else {
      set_NZ_unknown();
   }
   return -1;
}

static int op_XCE(operand_t operand, ea_t ea) {
   int tmp = C;
   C = E;
   E = tmp;
   if (tmp < 0) {
      MS = -1;
      XS = -1;
      E = -1;
   } else if (tmp > 0) {
      emulation_mode_on();
   } else {
      emulation_mode_off();
   }
   return -1;
}

static void repsep(int operand, int val) {
   if (operand & 0x80) {
      N = val;
   }
   if (operand & 0x40) {
      V = val;
   }
   if (E == 0) {
      if (operand & 0x20) {
         MS = val;
      }
      if (operand & 0x10) {
         XS = val;
         x_flag_updated();
      }
   }
   if (operand & 0x08) {
      D = val;
   }
   if (operand & 0x04) {
      I = val;
   }
   if (operand & 0x02) {
      Z = val;
   }
   if (operand & 0x01) {
      C = val;
   }
}

// Reset/Set Processor Status Bits
static int op_REP(operand_t operand, ea_t ea) {
   repsep(operand, 0);
   return -1;
}

static int op_SEP(operand_t operand, ea_t ea) {
   repsep(operand, 1);
   return -1;
}

// Jump to Subroutine Long
static int op_JSL(operand_t operand, ea_t ea) {
   // JSR: the operand is the data pushed to the stack (PB, PCH, PCL)
   push8(operand >> 16); // PB
   push16(operand);      // PC
   return -1;
}

// Return from Subroutine Long
static int op_RTL(operand_t operand, ea_t ea) {
   // RTL: the operand is the data pulled from the stack (PCL, PCH, PB)
   pop24new(operand);
   // The +1 is handled elsewhere
   PC = operand & 0xffff;
   PB = (operand >> 16) & 0xff;
   return -1;
}

// ====================================================================
// 65816/6502 instructions
// ====================================================================

static int op_ADC(operand_t operand, ea_t ea) {
   int acc = get_accumulator();
   if (acc >= 0 && C >= 0) {
      int tmp = 0;
      if (D == 1) {
         // Decimal mode ADC - works like a 65C02
         // Working a nibble at a time, correct for both 8 and 18 bits
         for (int bit = 0; bit < (MS ? 8 : 16); bit += 4) {
            int an = (acc >> bit) & 0xF;
            int bn = (operand >> bit) & 0xF;
            int rn =  an + bn + C;
            V = ((rn ^ an) & 8) && !((bn ^ an) & 8);
            C = 0;
            if (rn >= 10) {
               rn = (rn - 10) & 0xF;
               C = 1;
            }
            tmp |= rn << bit;
         }
      } else {
         // Normal mode ADC
         tmp = acc + operand + C;
         if (MS > 0) {
            // 8-bit mode
            C = (tmp >> 8) & 1;
            V = (((acc ^ operand) & 0x80) == 0) && (((acc ^ tmp) & 0x80) != 0);
         } else {
            // 16-bit mode
            C = (tmp >> 16) & 1;
            V = (((acc ^ operand) & 0x8000) == 0) && (((acc ^ tmp) & 0x8000) != 0);
         }
      }
      if (MS > 0) {
         // 8-bit mode
         A = tmp & 0xff;
      } else {
         // 16-bit mode
         A = tmp & 0xff;
         B = (tmp >> 8) & 0xff;
      }
      set_NZ_AB(A, B);
   } else {
      A = -1;
      B = -1;
      set_NVZC_unknown();
   }
   return -1;
}

static int op_AND(operand_t operand, ea_t ea) {
   // A is always updated, regardless of the size
   if (A >= 0) {
      A = A & (operand & 0xff);
   }
   // B is updated only of the size is 16
   if (B >= 0) {
      if (MS == 0) {
         B = B & (operand >> 8);
      } else if (MS < 0) {
         B = -1;
      }
   }
   // Updating NZ is complex, depending on the whether A and/or B are unknown
   set_NZ_AB(A, B);
   return -1;
}

static int op_ASLA(operand_t operand, ea_t ea) {
   // Compute the new carry
   if (MS > 0 && A >= 0) {
      // 8-bit mode
      C = (A >> 7) & 1;
   } else if (MS == 0 && B >= 0) {
      // 16-bit mode
      C = (B >> 7) & 1;
   } else {
      // width unknown
      C = -1;
   }
   // Compute the new B
   if (MS == 0 && B >= 0) {
      if (A >= 0) {
         B = ((B << 1) & 0xfe) | ((A >> 7) & 1);
      } else {
         B = -1;
      }
   } else if (MS < 0) {
      B = -1;
   }
   // Compute the new A
   if (A >= 0) {
      A = (A << 1) & 0xff;
   }
   // Updating NZ is complex, depending on the whether A and/or B are unknown
   set_NZ_AB(A, B);
   return -1;
}

static int op_ASL(operand_t operand, ea_t ea) {
   int tmp;
   if (MS > 0) {
      // 8-bit mode
      C = (operand >> 7) & 1;
      tmp = (operand << 1) & 0xff;
      set_NZ8(tmp);
   } else if (MS == 0) {
      // 16-bit mode
      C = (operand >> 15) & 1;
      tmp = (operand << 1) & 0xffff;
      set_NZ16(tmp);
   } else {
      // mode unknown
      C = -1;
      tmp = -1;
      set_NZ_unknown();
   }
   return tmp;
}

static int op_BCC(operand_t branch_taken, ea_t ea) {
   if (C >= 0) {
      if (C == branch_taken) {
         failflag = 1;
      }
   } else {
      C = 1 - branch_taken;
   }
   return -1;
}

static int op_BCS(operand_t branch_taken, ea_t ea) {
   if (C >= 0) {
      if (C != branch_taken) {
         failflag = 1;
      }
   } else {
      C = branch_taken;
   }
   return -1;
}

static int op_BNE(operand_t branch_taken, ea_t ea) {
   if (Z >= 0) {
      if (Z == branch_taken) {
         failflag = 1;
      }
   } else {
      Z = 1 - branch_taken;
   }
   return -1;
}

static int op_BEQ(operand_t branch_taken, ea_t ea) {
   if (Z >= 0) {
      if (Z != branch_taken) {
         failflag = 1;
        }
   } else {
      Z = branch_taken;
   }
   return -1;
}

static int op_BPL(operand_t branch_taken, ea_t ea) {
   if (N >= 0) {
      if (N == branch_taken) {
         failflag = 1;
      }
   } else {
      N = 1 - branch_taken;
   }
   return -1;
}

static int op_BMI(operand_t branch_taken, ea_t ea) {
   if (N >= 0) {
      if (N != branch_taken) {
         failflag = 1;
      }
   } else {
      N = branch_taken;
   }
   return -1;
}

static int op_BVC(operand_t branch_taken, ea_t ea) {
   if (V >= 0) {
      if (V == branch_taken) {
         failflag = 1;
      }
   } else {
      V = 1 - branch_taken;
   }
   return -1;
}

static int op_BVS(operand_t branch_taken, ea_t ea) {
   if (V >= 0) {
      if (V != branch_taken) {
         failflag = 1;
        }
   } else {
      V = branch_taken;
   }
   return -1;
}

static int op_BIT_IMM(operand_t operand, ea_t ea) {
   int acc = get_accumulator();
   if (operand == 0) {
      // This makes the remainder less pessimistic
      Z = 1;
   } else if (acc >= 0) {
      // both acc and operand will be the correct width
      Z = (acc & operand) == 0;
   } else {
      Z = -1;
   }
   return -1;
}

static int op_BIT(operand_t operand, ea_t ea) {
   if (MS > 0) {
      // 8-bit mode
      N = (operand >> 7) & 1;
      V = (operand >> 6) & 1;
   } else if (MS == 0) {
      // 16-bit mode
      N = (operand >> 15) & 1;
      V = (operand >> 14) & 1;
   } else {
      // mode undefined
      N = -1; // could be less pessimistic
      V = -1; // could be less pessimistic
   }
   // the rest is the same as BIT immediate (i.e. setting the Z flag)
   return op_BIT_IMM(operand, ea);
}

static int op_CLC(operand_t operand, ea_t ea) {
   C = 0;
   return -1;
}

static int op_CLD(operand_t operand, ea_t ea) {
   D = 0;
   return -1;
}

static int op_CLI(operand_t operand, ea_t ea) {
   I = 0;
   return -1;
}

static int op_CLV(operand_t operand, ea_t ea) {
   V = 0;
   return -1;
}

static int op_CMP(operand_t operand, ea_t ea) {
   int acc = get_accumulator();
   if (acc >= 0) {
      int tmp = acc - operand;
      C = tmp >= 0;
      set_NZ_MS(tmp);
   } else {
      set_NZC_unknown();
   }
   return -1;
}

static int op_CPX(operand_t operand, ea_t ea) {
   if (X >= 0) {
      int tmp = X - operand;
      C = tmp >= 0;
      set_NZ_XS(tmp);
   } else {
      set_NZC_unknown();
   }
   return -1;
}

static int op_CPY(operand_t operand, ea_t ea) {
   if (Y >= 0) {
      int tmp = Y - operand;
      C = tmp >= 0;
      set_NZ_XS(tmp);
   } else {
      set_NZC_unknown();
   }
   return -1;
}

static int op_DECA(operand_t operand, ea_t ea) {
   // Compute the new A
   if (A >= 0) {
      A = (A - 1) & 0xff;
   }
   // Compute the new B
   if (MS == 0 && B >= 0) {
      if (A == 0xff) {
         B = (B - 1) & 0xff;
      } else if (A < 0) {
         B = -1;
      }
   } else if (MS < 0) {
      B = -1;
   }
   // Updating NZ is complex, depending on the whether A and/or B are unknown
   set_NZ_AB(A, B);
   return -1;
}

static int op_DEC(operand_t operand, ea_t ea) {
   int tmp = -1;
   if (MS > 0) {
      // 8-bit mode
      tmp = (operand - 1) & 0xff;
      set_NZ8(tmp);
   } else if (MS == 0) {
      // 16-bit mode
      tmp = (operand - 1) & 0xffff;
      set_NZ16(tmp);
   } else {
      set_NZ_unknown();
   }
   return tmp;
}

static int op_DEX(operand_t operand, ea_t ea) {
   if (X >= 0) {
      if (XS > 0) {
         // 8-bit mode
         X = (X - 1) & 0xff;
         set_NZ8(X);
      } else if (XS == 0) {
         // 16-bit mode
         X = (X - 1) & 0xffff;
         set_NZ16(X);
      } else {
         // mode undefined
         X = -1;
         set_NZ_unknown();
      }
   } else {
      set_NZ_unknown();
   }
   return -1;
}

static int op_DEY(operand_t operand, ea_t ea) {
   if (Y >= 0) {
      if (XS > 0) {
         // 8-bit mode
         Y = (Y - 1) & 0xff;
         set_NZ8(Y);
      } else if (XS == 0) {
         // 16-bit mode
         Y = (Y - 1) & 0xffff;
         set_NZ16(Y);
      } else {
         // mode undefined
         Y = -1;
         set_NZ_unknown();
      }
   } else {
      set_NZ_unknown();
   }
   return -1;
}

static int op_EOR(operand_t operand, ea_t ea) {
   // A is always updated, regardless of the size
   if (A >= 0) {
      A = A ^ (operand & 0xff);
   }
   // B is updated only of the size is 16
   if (B >= 0) {
      if (MS == 0) {
         B = B ^ (operand >> 8);
      } else if (MS < 0) {
         B = -1;
      }
   }
   // Updating NZ is complex, depending on the whether A and/or B are unknown
   set_NZ_AB(A, B);
   return -1;
}

static int op_INCA(operand_t operand, ea_t ea) {
   // Compute the new A
   if (A >= 0) {
      A = (A + 1) & 0xff;
   }
   // Compute the new B
   if (MS == 0 && B >= 0) {
      if (A == 0x00) {
         B = (B + 1) & 0xff;
      } else if (A < 0) {
         B = -1;
      }
   } else if (MS < 0) {
      B = -1;
   }
   // Updating NZ is complex, depending on the whether A and/or B are unknown
   set_NZ_AB(A, B);
   return -1;
}

static int op_INC(operand_t operand, ea_t ea) {
   int tmp = -1;
   if (MS > 0) {
      // 8-bit mode
      tmp = (operand + 1) & 0xff;
      set_NZ8(tmp);
   } else if (MS == 0) {
      // 16-bit mode
      tmp = (operand + 1) & 0xffff;
      set_NZ16(tmp);
   } else {
      set_NZ_unknown();
   }
   return tmp;
}

static int op_INX(operand_t operand, ea_t ea) {
   if (X >= 0) {
      if (XS > 0) {
         // 8-bit mode
         X = (X + 1) & 0xff;
         set_NZ8(X);
      } else if (XS == 0) {
         // 16-bit mode
         X = (X + 1) & 0xffff;
         set_NZ16(X);
      } else {
         // mode undefined
         X = -1;
         set_NZ_unknown();
      }
   } else {
      set_NZ_unknown();
   }
   return -1;
}

static int op_INY(operand_t operand, ea_t ea) {
   if (Y >= 0) {
      if (XS > 0) {
         // 8-bit mode
         Y = (Y + 1) & 0xff;
         set_NZ8(Y);
      } else if (XS == 0) {
         // 16-bit mode
         Y = (Y + 1) & 0xffff;
         set_NZ16(Y);
      } else {
         // mode undefined
         Y = -1;
         set_NZ_unknown();
      }
   } else {
      set_NZ_unknown();
   }
   return -1;
}

static int op_JSR(operand_t operand, ea_t ea) {
   // JSR: the operand is the data pushed to the stack (PCH, PCL)
   push16(operand);  // PC
   return -1;
}

static int op_JSR_new(operand_t operand, ea_t ea) {
   // JSR: the operand is the data pushed to the stack (PCH, PCL)
   push16new(operand);  // PC
   return -1;
}

static int op_LDA(operand_t operand, ea_t ea) {
   A = operand & 0xff;
   if (MS == 0) {
      B = (operand >> 8) & 0xff;
   }
   set_NZ_AB(A, B);
   return -1;
}

static int op_LDX(operand_t operand, ea_t ea) {
   X = operand;
   set_NZ_XS(X);
   return -1;
}

static int op_LDY(operand_t operand, ea_t ea) {
   Y = operand;
   set_NZ_XS(Y);
   return -1;
}

static int op_LSRA(operand_t operand, ea_t ea) {
   // Compute the new carry
   if (A >= 0) {
      C = A & 1;
   } else {
      C = -1;
   }
   // Compute the new A
   if (MS > 0 && A >= 0) {
      A = A >> 1;
   } else if (MS == 0 && A >= 0 && B >= 0) {
      A = ((A >> 1) | (B << 7)) & 0xff;
   } else {
      A = -1;
   }
   // Compute the new B
   if (MS == 0 && B >= 0) {
      B = (B >> 1) & 0xff;
   } else if (MS < 0) {
      B = -1;
   }
   // Updating NZ is complex, depending on the whether A and/or B are unknown
   set_NZ_AB(A, B);
   return -1;
}

static int op_LSR(operand_t operand, ea_t ea) {
   int tmp;
   C = operand & 1;
   if (MS > 0) {
      // 8-bit mode
      tmp = (operand >> 1) & 0xff;
      set_NZ8(tmp);
   } else if (MS == 0) {
      // 16-bit mode
      tmp = (operand >> 1) & 0xffff;
      set_NZ16(tmp);
   } else {
      // mode unknown
      tmp = -1;
      set_NZ_unknown();
   }
   return tmp;
}

static int op_ORA(operand_t operand, ea_t ea) {
   // A is always updated, regardless of the size
   if (A >= 0) {
      A = A | (operand & 0xff);
   }
   // B is updated only of the size is 16
   if (B >= 0) {
      if (MS == 0) {
         B = B | (operand >> 8);
      } else if (MS < 0) {
         B = -1;
      }
   }
   // Updating NZ is complex, depending on the whether A and/or B are unknown
   set_NZ_AB(A, B);
   return -1;
}

static int op_PHA(operand_t operand, ea_t ea) {
   pushMS(operand);
   op_STA(operand, -1);
   return -1;
}

static int op_PHP(operand_t operand, ea_t ea) {
   push8(operand);
   check_FLAGS(operand);
   set_FLAGS(operand);
   return -1;
}

static int op_PHX(operand_t operand, ea_t ea) {
   pushXS(operand);
   op_STX(operand, -1);
   return -1;
}

static int op_PHY(operand_t operand, ea_t ea) {
   pushXS(operand);
   op_STY(operand, -1);
   return -1;
}

static int op_PLA(operand_t operand, ea_t ea) {
   A = operand & 0xff;
   if (MS < 0) {
      B = -1;
   } else if (MS == 0) {
      B = (operand >> 8);
   }
   set_NZ_MS(operand);
   popMS(operand);
   return -1;
}

static int op_PLP(operand_t operand, ea_t ea) {
   set_FLAGS(operand);
   pop8(operand);
   return -1;
}

static int op_PLX(operand_t operand, ea_t ea) {
   X = operand;
   set_NZ_XS(X);
   popXS(operand);
   return -1;
}

static int op_PLY(operand_t operand, ea_t ea) {
   Y = operand;
   set_NZ_XS(Y);
   popXS(operand);
   return -1;
}

static int op_ROLA(operand_t operand, ea_t ea) {
   // Save the old carry
   int oldC = C;
   // Compute the new carry
   if (MS > 0 && A >= 0) {
      // 8-bit mode
      C = (A >> 7) & 1;
   } else if (MS == 0 && B >= 0) {
      // 16-bit mode
      C = (B >> 7) & 1;
   } else {
      // width unknown
      C = -1;
   }
   // Compute the new B
   if (MS == 0 && B >= 0) {
      if (A >= 0) {
         B = ((B << 1) & 0xfe) | ((A >> 7) & 1);
      } else {
         B = -1;
      }
   } else if (MS < 0) {
      B = -1;
   }
   // Compute the new A
   if (A >= 0) {
      if (oldC >= 0) {
         A = ((A << 1) | oldC) & 0xff;
      } else {
         A = -1;
      }
   }
   // Updating NZ is complex, depending on the whether A and/or B are unknown
   set_NZ_AB(A, B);
   return -1;
}

static int op_ROL(operand_t operand, ea_t ea) {
   int oldC = C;
   int tmp;
   if (MS > 0) {
      // 8-bit mode
      C = (operand >> 7) & 1;
      tmp = ((operand << 1) | oldC) & 0xff;
      set_NZ8(tmp);
   } else if (MS == 0) {
      // 16-bit mode
      C = (operand >> 15) & 1;
      tmp = ((operand << 1) | oldC) & 0xffff;
      set_NZ16(tmp);
   } else {
      C = -1;
      tmp = -1;
      set_NZ_unknown();
   }
   return tmp;
}

static int op_RORA(operand_t operand, ea_t ea) {
   // Save the old carry
   int oldC = C;
   // Compute the new carry
   if (A >= 0) {
      C = A & 1;
   } else {
      C = -1;
   }
   // Compute the new A
   if (MS > 0 && A >= 0) {
      A = ((A >> 1) | (oldC << 7)) & 0xff;
   } else if (MS == 0 && A >= 0 && B >= 0) {
      A = ((A >> 1) | (B << 7)) & 0xff;
   } else {
      A = -1;
   }
   // Compute the new B
   if (MS == 0 && B >= 0 && oldC >= 0) {
      B = ((B >> 1) | (oldC << 7)) & 0xff;
   } else if (MS < 0) {
      B = -1;
   }
   // Updating NZ is complex, depending on the whether A and/or B are unknown
   set_NZ_AB(A, B);
   return -1;
}

static int op_ROR(operand_t operand, ea_t ea) {
   int oldC = C;
   int tmp;
   C = operand & 1;
   if (MS > 0) {
      // 8-bit mode
      tmp = ((operand >> 1) | (oldC << 7)) & 0xff;
      set_NZ8(tmp);
   } else if (MS == 0) {
      // 16-bit mode
      tmp = ((operand >> 1) | (oldC << 15)) & 0xffff;
      set_NZ16(tmp);
   } else {
      C = -1;
      tmp = -1;
      set_NZ_unknown();
   }
   return tmp;
}

static int op_RTS(operand_t operand, ea_t ea) {
   // RTS: the operand is the data pulled from the stack (PCL, PCH)
   pop8(operand);
   pop8(operand >> 8);
   // The +1 is handled elsewhere
   PC = operand & 0xffff;
   return -1;
}

static int op_RTI(operand_t operand, ea_t ea) {
   // RTI: the operand is the data pulled from the stack (P, PCL, PCH, PBR)
   set_FLAGS(operand);
   pop8(operand);
   pop8(operand >> 8);
   pop8(operand >> 16);
   if (E == 0) {
      pop8(operand >> 24);
   }
   return -1;
}

static int op_SBC(operand_t operand, ea_t ea) {
   int acc = get_accumulator();
   if (acc >= 0 && C >= 0) {
      int tmp = 0;
      if (D == 1) {
         // Decimal mode SBC - works like a 65C02
         // Working a nibble at a time, correct for both 8 and 18 bits
         for (int bit = 0; bit < (MS ? 8 : 16); bit += 4) {
            int an = (acc >> bit) & 0xF;
            int bn = (operand >> bit) & 0xF;
            int rn =  an - bn - (1 - C);
            V = ((rn ^ an) & 8) && ((bn ^ an) & 8);
            C = 1;
            if (rn < 0) {
               rn = (rn + 10) & 0xF;
               C = 0;
            }
            tmp |= rn << bit;
         }
      } else {
         // Normal mode SBC
         tmp = acc - operand - (1 - C);
         if (MS > 0) {
            // 8-bit mode
            C = 1 - ((tmp >> 8) & 1);
            V = (((acc ^ operand) & 0x80) != 0) && (((acc ^ tmp) & 0x80) != 0);
         } else {
            // 16-bit mode
            C = 1 - ((tmp >> 16) & 1);
            V = (((acc ^ operand) & 0x8000) != 0) && (((acc ^ tmp) & 0x8000) != 0);
         }
      }
      if (MS > 0) {
         // 8-bit mode
         A = tmp & 0xff;
      } else {
         // 16-bit mode
         A = tmp & 0xff;
         B = (tmp >> 8) & 0xff;
      }
      set_NZ_AB(A, B);
   } else {
      A = -1;
      B = -1;
      set_NVZC_unknown();
   }
   return -1;
}

static int op_SEC(operand_t operand, ea_t ea) {
   C = 1;
   return -1;
}

static int op_SED(operand_t operand, ea_t ea) {
   D = 1;
   return -1;
}

static int op_SEI(operand_t operand, ea_t ea) {
   I = 1;
   return -1;
}

static int op_STA(operand_t operand, ea_t ea) {
   int oplo = operand & 0xff;
   int ophi = (operand >> 8) & 0xff;
   // Always write A
   if (A >= 0) {
      if (oplo != A) {
         failflag = 1;
      }
   }
   A = oplo;
   // Optionally write B, depending on the MS flag
   if (MS < 0) {
      B = -1;
   } else if (MS == 0) {
      if (B >= 0) {
         if (ophi != B) {
            failflag = 1;
         }
      }
      B = ophi;
   }
   return operand;
}

static int op_STX(operand_t operand, ea_t ea) {
   if (X >= 0) {
      if (operand != X) {
         failflag = 1;
      }
   }
   X = operand;
   return operand;
}

static int op_STY(operand_t operand, ea_t ea) {
   if (Y >= 0) {
      if (operand != Y) {
         failflag = 1;
      }
   }
   Y = operand;
   return operand;
}

static int op_STZ(operand_t operand, ea_t ea) {
   if (operand != 0) {
      failflag = 1;
   }
   return operand;
}


static int op_TSB(operand_t operand, ea_t ea) {
   int acc = get_accumulator();
   if (acc >= 0) {
      Z = ((acc & operand) == 0);
      return operand | acc;
   } else {
      Z = -1;
      return -1;
   }
}

static int op_TRB(operand_t operand, ea_t ea) {
   int acc = get_accumulator();
   if (acc >= 0) {
      Z = ((acc & operand) == 0);
      return operand &~ acc;
   } else {
      Z = -1;
      return -1;
   }
}

// This is used to implement: TAX, TAY, TSX
static void transfer_88_16(int srchi, int srclo, int *dst) {
   if (srclo >= 0 && srchi >=0 && XS == 0) {
      // 16-bit
      *dst = (srchi << 8) + srclo;
      set_NZ16(*dst);
   } else if (srclo >= 0 && XS == 1) {
      // 8-bit
      *dst = srclo;
      set_NZ8(*dst);
   } else {
      *dst = -1;
      set_NZ_unknown();
   }
}

// This is used to implement: TXA, TYA
static void transfer_16_88(int src, int *dsthi, int *dstlo) {
   if (MS == 0) {
      // 16-bit
      if (src >= 0) {
         *dsthi = (src >> 8) & 0xff;
         *dstlo = src & 0xff;
         set_NZ16(src);
      } else {
         *dsthi = -1;
         *dstlo = -1;
         set_NZ_unknown();
      }
   } else if (MS == 1) {
      // 8-bit
      if (src >= 0) {
         *dstlo = src & 0xff;
         set_NZ8(src);
      } else {
         *dstlo = -1;
         set_NZ_unknown();
      }
   } else {
      // MS undefined
      if (src >= 0) {
         *dstlo = src & 0xff;
      } else {
         *dstlo = -1;
      }
      *dsthi = -1;
      set_NZ_unknown();
   }
}

static int op_TAX(operand_t operand, ea_t ea) {
   transfer_88_16(B, A, &X);
   return -1;
}

static int op_TAY(operand_t operand, ea_t ea) {
   transfer_88_16(B, A, &Y);
   return -1;
}

static int op_TSX(operand_t operand, ea_t ea) {
   transfer_88_16(SH, SL, &X);
   return -1;
}

static int op_TXA(operand_t operand, ea_t ea) {
   transfer_16_88(X, &B, &A);
   return -1;
}

static int op_TXS(operand_t operand, ea_t ea) {
   if (X >= 0) {
      SH = (X >> 8) & 0xff;
      SL = (X     ) & 0xff;
   } else {
      SH = -1;
      SL = -1;
   }
   // Force SH to be 1 in emulation mode
   if (E == 1) {
      SH = 1;
   } else if (E < 0 && SH != 1) {
      SH = -1;
   }
   return -1;
}

static int op_TYA(operand_t operand, ea_t ea) {
   transfer_16_88(Y, &B, &A);
   return -1;
}

// ====================================================================
// Opcode Tables
// ====================================================================


static InstrType instr_table_65c816[] = {
   /* 00 */   { "BRK",  0, IMM   , 7, 0, OTHER,    0},
   /* 01 */   { "ORA",  0, INDX  , 6, 0, READOP,   op_ORA},
   /* 02 */   { "COP",  0, IMM   , 7, 1, OTHER,    0},
   /* 03 */   { "ORA",  0, SR    , 4, 1, READOP,   op_ORA},
   /* 04 */   { "TSB",  0, ZP    , 5, 0, RMWOP,    op_TSB},
   /* 05 */   { "ORA",  0, ZP    , 3, 0, READOP,   op_ORA},
   /* 06 */   { "ASL",  0, ZP    , 5, 0, RMWOP,    op_ASL},
   /* 07 */   { "ORA",  0, IDL   , 6, 1, READOP,   op_ORA},
   /* 08 */   { "PHP",  0, IMP   , 3, 0, OTHER,    op_PHP},
   /* 09 */   { "ORA",  0, IMM   , 2, 0, OTHER,    op_ORA},
   /* 0A */   { "ASL",  0, IMPA  , 2, 0, OTHER,    op_ASLA},
   /* 0B */   { "PHD",  0, IMP   , 4, 1, OTHER,    op_PHD},
   /* 0C */   { "TSB",  0, ABS   , 6, 0, RMWOP,    op_TSB},
   /* 0D */   { "ORA",  0, ABS   , 4, 0, READOP,   op_ORA},
   /* 0E */   { "ASL",  0, ABS   , 6, 0, RMWOP,    op_ASL},
   /* 0F */   { "ORA",  0, ABL   , 5, 1, READOP,   op_ORA},
   /* 10 */   { "BPL",  0, BRA   , 2, 0, BRANCHOP, op_BPL},
   /* 11 */   { "ORA",  0, INDY  , 5, 0, READOP,   op_ORA},
   /* 12 */   { "ORA",  0, IND   , 5, 0, READOP,   op_ORA},
   /* 13 */   { "ORA",  0, ISY   , 7, 1, READOP,   op_ORA},
   /* 14 */   { "TRB",  0, ZP    , 5, 0, RMWOP,    op_TRB},
   /* 15 */   { "ORA",  0, ZPX   , 4, 0, READOP,   op_ORA},
   /* 16 */   { "ASL",  0, ZPX   , 6, 0, RMWOP,    op_ASL},
   /* 17 */   { "ORA",  0, IDLY  , 6, 1, READOP,   op_ORA},
   /* 18 */   { "CLC",  0, IMP   , 2, 0, OTHER,    op_CLC},
   /* 19 */   { "ORA",  0, ABSY  , 4, 0, READOP,   op_ORA},
   /* 1A */   { "INC",  0, IMPA  , 2, 0, OTHER,    op_INCA},
   /* 1B */   { "TCS",  0, IMP   , 2, 1, OTHER,    op_TCS},
   /* 1C */   { "TRB",  0, ABS   , 6, 0, RMWOP,    op_TRB},
   /* 1D */   { "ORA",  0, ABSX  , 4, 0, READOP,   op_ORA},
   /* 1E */   { "ASL",  0, ABSX  , 7, 0, RMWOP,    op_ASL},
   /* 1F */   { "ORA",  0, ALX   , 5, 1, READOP,   op_ORA},
   /* 20 */   { "JSR",  0, ABS   , 6, 0, OTHER,    op_JSR},
   /* 21 */   { "AND",  0, INDX  , 6, 0, READOP,   op_AND},
   /* 22 */   { "JSL",  0, ABL   , 8, 1, OTHER,    op_JSL},
   /* 23 */   { "AND",  0, SR    , 4, 1, READOP,   op_AND},
   /* 24 */   { "BIT",  0, ZP    , 3, 0, READOP,   op_BIT},
   /* 25 */   { "AND",  0, ZP    , 3, 0, READOP,   op_AND},
   /* 26 */   { "ROL",  0, ZP    , 5, 0, RMWOP,    op_ROL},
   /* 27 */   { "AND",  0, IDL   , 6, 1, READOP,   op_AND},
   /* 28 */   { "PLP",  0, IMP   , 4, 0, OTHER,    op_PLP},
   /* 29 */   { "AND",  0, IMM   , 2, 0, OTHER,    op_AND},
   /* 2A */   { "ROL",  0, IMPA  , 2, 0, OTHER,    op_ROLA},
   /* 2B */   { "PLD",  0, IMP   , 5, 1, OTHER,    op_PLD},
   /* 2C */   { "BIT",  0, ABS   , 4, 0, READOP,   op_BIT},
   /* 2D */   { "AND",  0, ABS   , 4, 0, READOP,   op_AND},
   /* 2E */   { "ROL",  0, ABS   , 6, 0, RMWOP,    op_ROL},
   /* 2F */   { "AND",  0, ABL   , 5, 1, READOP,   op_AND},
   /* 30 */   { "BMI",  0, BRA   , 2, 0, BRANCHOP, op_BMI},
   /* 31 */   { "AND",  0, INDY  , 5, 0, READOP,   op_AND},
   /* 32 */   { "AND",  0, IND   , 5, 0, READOP,   op_AND},
   /* 33 */   { "AND",  0, ISY   , 7, 1, READOP,   op_AND},
   /* 34 */   { "BIT",  0, ZPX   , 4, 0, READOP,   op_BIT},
   /* 35 */   { "AND",  0, ZPX   , 4, 0, READOP,   op_AND},
   /* 36 */   { "ROL",  0, ZPX   , 6, 0, RMWOP,    op_ROL},
   /* 37 */   { "AND",  0, IDLY  , 6, 1, READOP,   op_AND},
   /* 38 */   { "SEC",  0, IMP   , 2, 0, OTHER,    op_SEC},
   /* 39 */   { "AND",  0, ABSY  , 4, 0, READOP,   op_AND},
   /* 3A */   { "DEC",  0, IMPA  , 2, 0, OTHER,    op_DECA},
   /* 3B */   { "TSC",  0, IMP   , 2, 1, OTHER,    op_TSC},
   /* 3C */   { "BIT",  0, ABSX  , 4, 0, READOP,   op_BIT},
   /* 3D */   { "AND",  0, ABSX  , 4, 0, READOP,   op_AND},
   /* 3E */   { "ROL",  0, ABSX  , 7, 0, RMWOP,    op_ROL},
   /* 3F */   { "AND",  0, ALX   , 5, 1, READOP,   op_AND},
   /* 40 */   { "RTI",  0, IMP   , 6, 0, OTHER,    op_RTI},
   /* 41 */   { "EOR",  0, INDX  , 6, 0, READOP,   op_EOR},
   /* 42 */   { "WDM",  0, IMM   , 2, 1, OTHER,    0},
   /* 43 */   { "EOR",  0, SR    , 4, 1, READOP,   op_EOR},
   /* 44 */   { "MVP",  0, BM    , 7, 1, OTHER,    op_MVP},
   /* 45 */   { "EOR",  0, ZP    , 3, 0, READOP,   op_EOR},
   /* 46 */   { "LSR",  0, ZP    , 5, 0, RMWOP,    op_LSR},
   /* 47 */   { "EOR",  0, IDL   , 6, 1, READOP,   op_EOR},
   /* 48 */   { "PHA",  0, IMP   , 3, 0, OTHER,    op_PHA},
   /* 49 */   { "EOR",  0, IMM   , 2, 0, OTHER,    op_EOR},
   /* 4A */   { "LSR",  0, IMPA  , 2, 0, OTHER,    op_LSRA},
   /* 4B */   { "PHK",  0, IMP   , 3, 1, OTHER,    op_PHK},
   /* 4C */   { "JMP",  0, ABS   , 3, 0, OTHER,    0},
   /* 4D */   { "EOR",  0, ABS   , 4, 0, READOP,   op_EOR},
   /* 4E */   { "LSR",  0, ABS   , 6, 0, RMWOP,    op_LSR},
   /* 4F */   { "EOR",  0, ABL   , 5, 1, READOP,   op_EOR},
   /* 50 */   { "BVC",  0, BRA   , 2, 0, BRANCHOP, op_BVC},
   /* 51 */   { "EOR",  0, INDY  , 5, 0, READOP,   op_EOR},
   /* 52 */   { "EOR",  0, IND   , 5, 0, READOP,   op_EOR},
   /* 53 */   { "EOR",  0, ISY   , 7, 1, READOP,   op_EOR},
   /* 54 */   { "MVN",  0, BM    , 7, 1, OTHER,    op_MVN},
   /* 55 */   { "EOR",  0, ZPX   , 4, 0, READOP,   op_EOR},
   /* 56 */   { "LSR",  0, ZPX   , 6, 0, RMWOP,    op_LSR},
   /* 57 */   { "EOR",  0, IDLY  , 6, 1, READOP,   op_EOR},
   /* 58 */   { "CLI",  0, IMP   , 2, 0, OTHER,    op_CLI},
   /* 59 */   { "EOR",  0, ABSY  , 4, 0, READOP,   op_EOR},
   /* 5A */   { "PHY",  0, IMP   , 3, 0, OTHER,    op_PHY},
   /* 5B */   { "TCD",  0, IMP   , 2, 1, OTHER,    op_TCD},
   /* 5C */   { "JML",  0, ABL   , 4, 1, OTHER,    0},
   /* 5D */   { "EOR",  0, ABSX  , 4, 0, READOP,   op_EOR},
   /* 5E */   { "LSR",  0, ABSX  , 7, 0, RMWOP,    op_LSR},
   /* 5F */   { "EOR",  0, ALX   , 5, 1, READOP,   op_EOR},
   /* 60 */   { "RTS",  0, IMP   , 6, 0, OTHER,    op_RTS},
   /* 61 */   { "ADC",  0, INDX  , 6, 0, READOP,   op_ADC},
   /* 62 */   { "PER",  0, BRL   , 6, 1, OTHER,    op_PER},
   /* 63 */   { "ADC",  0, SR    , 4, 1, READOP,   op_ADC},
   /* 64 */   { "STZ",  0, ZP    , 3, 0, WRITEOP,  op_STZ},
   /* 65 */   { "ADC",  0, ZP    , 3, 0, READOP,   op_ADC},
   /* 66 */   { "ROR",  0, ZP    , 5, 0, RMWOP,    op_ROR},
   /* 67 */   { "ADC",  0, IDL   , 6, 1, READOP,   op_ADC},
   /* 68 */   { "PLA",  0, IMP   , 4, 0, OTHER,    op_PLA},
   /* 69 */   { "ADC",  0, IMM   , 2, 0, OTHER,    op_ADC},
   /* 6A */   { "ROR",  0, IMPA  , 2, 0, OTHER,    op_RORA},
   /* 6B */   { "RTL",  0, IMP   , 6, 1, OTHER,    op_RTL},
   /* 6C */   { "JMP",  0, IND16 , 5, 0, OTHER,    0},
   /* 6D */   { "ADC",  0, ABS   , 4, 0, READOP,   op_ADC},
   /* 6E */   { "ROR",  0, ABS   , 6, 0, RMWOP,    op_ROR},
   /* 6F */   { "ADC",  0, ABL   , 5, 1, READOP,   op_ADC},
   /* 70 */   { "BVS",  0, BRA   , 2, 0, BRANCHOP, op_BVS},
   /* 71 */   { "ADC",  0, INDY  , 5, 0, READOP,   op_ADC},
   /* 72 */   { "ADC",  0, IND   , 5, 0, READOP,   op_ADC},
   /* 73 */   { "ADC",  0, ISY   , 7, 1, READOP,   op_ADC},
   /* 74 */   { "STZ",  0, ZPX   , 4, 0, WRITEOP,  op_STZ},
   /* 75 */   { "ADC",  0, ZPX   , 4, 0, READOP,   op_ADC},
   /* 76 */   { "ROR",  0, ZPX   , 6, 0, RMWOP,    op_ROR},
   /* 77 */   { "ADC",  0, IDLY  , 6, 1, READOP,   op_ADC},
   /* 78 */   { "SEI",  0, IMP   , 2, 0, OTHER,    op_SEI},
   /* 79 */   { "ADC",  0, ABSY  , 4, 0, READOP,   op_ADC},
   /* 7A */   { "PLY",  0, IMP   , 4, 0, OTHER,    op_PLY},
   /* 7B */   { "TDC",  0, IMP   , 2, 1, OTHER,    op_TDC},
   /* 7C */   { "JMP",  0, IND1X , 6, 0, OTHER,    0},
   /* 7D */   { "ADC",  0, ABSX  , 4, 0, READOP,   op_ADC},
   /* 7E */   { "ROR",  0, ABSX  , 7, 0, RMWOP,    op_ROR},
   /* 7F */   { "ADC",  0, ALX   , 5, 1, READOP,   op_ADC},
   /* 80 */   { "BRA",  0, BRA   , 3, 0, OTHER,    0},
   /* 81 */   { "STA",  0, INDX  , 6, 0, WRITEOP,  op_STA},
   /* 82 */   { "BRL",  0, BRL   , 4, 1, OTHER,    0},
   /* 83 */   { "STA",  0, SR    , 4, 1, WRITEOP,  op_STA},
   /* 84 */   { "STY",  0, ZP    , 3, 0, WRITEOP,  op_STY},
   /* 85 */   { "STA",  0, ZP    , 3, 0, WRITEOP,  op_STA},
   /* 86 */   { "STX",  0, ZP    , 3, 0, WRITEOP,  op_STX},
   /* 87 */   { "STA" , 0, IDL   , 6, 1, WRITEOP,  op_STA},
   /* 88 */   { "DEY",  0, IMP   , 2, 0, OTHER,    op_DEY},
   /* 89 */   { "BIT",  0, IMM   , 2, 0, OTHER,    op_BIT_IMM},
   /* 8A */   { "TXA",  0, IMP   , 2, 0, OTHER,    op_TXA},
   /* 8B */   { "PHB",  0, IMP   , 3, 1, OTHER,    op_PHB},
   /* 8C */   { "STY",  0, ABS   , 4, 0, WRITEOP,  op_STY},
   /* 8D */   { "STA",  0, ABS   , 4, 0, WRITEOP,  op_STA},
   /* 8E */   { "STX",  0, ABS   , 4, 0, WRITEOP,  op_STX},
   /* 8F */   { "STA",  0, ABL   , 5, 1, WRITEOP,  op_STA},
   /* 90 */   { "BCC",  0, BRA   , 2, 0, BRANCHOP, op_BCC},
   /* 91 */   { "STA",  0, INDY  , 6, 0, WRITEOP,  op_STA},
   /* 92 */   { "STA",  0, IND   , 5, 0, WRITEOP,  op_STA},
   /* 93 */   { "STA",  0, ISY   , 7, 1, WRITEOP,  op_STA},
   /* 94 */   { "STY",  0, ZPX   , 4, 0, WRITEOP,  op_STY},
   /* 95 */   { "STA",  0, ZPX   , 4, 0, WRITEOP,  op_STA},
   /* 96 */   { "STX",  0, ZPY   , 4, 0, WRITEOP,  op_STX},
   /* 97 */   { "STA",  0, IDLY  , 6, 1, WRITEOP,  op_STA},
   /* 98 */   { "TYA",  0, IMP   , 2, 0, OTHER,    op_TYA},
   /* 99 */   { "STA",  0, ABSY  , 5, 0, WRITEOP,  op_STA},
   /* 9A */   { "TXS",  0, IMP   , 2, 0, OTHER,    op_TXS},
   /* 9B */   { "TXY",  0, IMP   , 2, 1, OTHER,    op_TXY},
   /* 9C */   { "STZ",  0, ABS   , 4, 0, WRITEOP,  op_STZ},
   /* 9D */   { "STA",  0, ABSX  , 5, 0, WRITEOP,  op_STA},
   /* 9E */   { "STZ",  0, ABSX  , 5, 0, WRITEOP,  op_STZ},
   /* 9F */   { "STA",  0, ALX   , 5, 1, WRITEOP,  op_STA},
   /* A0 */   { "LDY",  0, IMM   , 2, 0, OTHER,    op_LDY},
   /* A1 */   { "LDA",  0, INDX  , 6, 0, READOP,   op_LDA},
   /* A2 */   { "LDX",  0, IMM   , 2, 0, OTHER,    op_LDX},
   /* A3 */   { "LDA",  0, SR    , 4, 1, READOP,   op_LDA},
   /* A4 */   { "LDY",  0, ZP    , 3, 0, READOP,   op_LDY},
   /* A5 */   { "LDA",  0, ZP    , 3, 0, READOP,   op_LDA},
   /* A6 */   { "LDX",  0, ZP    , 3, 0, READOP,   op_LDX},
   /* A7 */   { "LDA",  0, IDL   , 6, 1, READOP,   op_LDA},
   /* A8 */   { "TAY",  0, IMP   , 2, 0, OTHER,    op_TAY},
   /* A9 */   { "LDA",  0, IMM   , 2, 0, OTHER,    op_LDA},
   /* AA */   { "TAX",  0, IMP   , 2, 0, OTHER,    op_TAX},
   /* AB */   { "PLB",  0, IMP   , 4, 1, OTHER,    op_PLB},
   /* AC */   { "LDY",  0, ABS   , 4, 0, READOP,   op_LDY},
   /* AD */   { "LDA",  0, ABS   , 4, 0, READOP,   op_LDA},
   /* AE */   { "LDX",  0, ABS   , 4, 0, READOP,   op_LDX},
   /* AF */   { "LDA",  0, ABL   , 5, 1, READOP,   op_LDA},
   /* B0 */   { "BCS",  0, BRA   , 2, 0, BRANCHOP, op_BCS},
   /* B1 */   { "LDA",  0, INDY  , 5, 0, READOP,   op_LDA},
   /* B2 */   { "LDA",  0, IND   , 5, 0, READOP,   op_LDA},
   /* B3 */   { "LDA",  0, ISY   , 7, 1, READOP,   op_LDA},
   /* B4 */   { "LDY",  0, ZPX   , 4, 0, READOP,   op_LDY},
   /* B5 */   { "LDA",  0, ZPX   , 4, 0, READOP,   op_LDA},
   /* B6 */   { "LDX",  0, ZPY   , 4, 0, READOP,   op_LDX},
   /* B7 */   { "LDA",  0, IDLY  , 6, 1, READOP,   op_LDA},
   /* B8 */   { "CLV",  0, IMP   , 2, 0, OTHER,    op_CLV},
   /* B9 */   { "LDA",  0, ABSY  , 4, 0, READOP,   op_LDA},
   /* BA */   { "TSX",  0, IMP   , 2, 0, OTHER,    op_TSX},
   /* BB */   { "TYX",  0, IMP   , 2, 1, OTHER,    op_TYX},
   /* BC */   { "LDY",  0, ABSX  , 4, 0, READOP,   op_LDY},
   /* BD */   { "LDA",  0, ABSX  , 4, 0, READOP,   op_LDA},
   /* BE */   { "LDX",  0, ABSY  , 4, 0, READOP,   op_LDX},
   /* BF */   { "LDA",  0, ALX   , 5, 1, READOP,   op_LDA},
   /* C0 */   { "CPY",  0, IMM   , 2, 0, OTHER,    op_CPY},
   /* C1 */   { "CMP",  0, INDX  , 6, 0, READOP,   op_CMP},
   /* C2 */   { "REP",  0, IMM   , 3, 1, OTHER,    op_REP},
   /* C3 */   { "CMP",  0, SR    , 4, 1, READOP,   op_CMP},
   /* C4 */   { "CPY",  0, ZP    , 3, 0, READOP,   op_CPY},
   /* C5 */   { "CMP",  0, ZP    , 3, 0, READOP,   op_CMP},
   /* C6 */   { "DEC",  0, ZP    , 5, 0, RMWOP,    op_DEC},
   /* C7 */   { "CMP",  0, IDL   , 6, 1, READOP,   op_CMP},
   /* C8 */   { "INY",  0, IMP   , 2, 0, OTHER,    op_INY},
   /* C9 */   { "CMP",  0, IMM   , 2, 0, OTHER,    op_CMP},
   /* CA */   { "DEX",  0, IMP   , 2, 0, OTHER,    op_DEX},
   /* CB */   { "WAI",  0, IMP   , 1, 1, OTHER,    0},        // WD65C02=3
   /* CC */   { "CPY",  0, ABS   , 4, 0, READOP,   op_CPY},
   /* CD */   { "CMP",  0, ABS   , 4, 0, READOP,   op_CMP},
   /* CE */   { "DEC",  0, ABS   , 6, 0, RMWOP,    op_DEC},
   /* CF */   { "CMP",  0, ABL   , 5, 1, READOP,   op_CMP},
   /* D0 */   { "BNE",  0, BRA   , 2, 0, BRANCHOP, op_BNE},
   /* D1 */   { "CMP",  0, INDY  , 5, 0, READOP,   op_CMP},
   /* D2 */   { "CMP",  0, IND   , 5, 0, READOP,   op_CMP},
   /* D3 */   { "CMP",  0, ISY   , 7, 1, READOP,   op_CMP},
   /* D4 */   { "PEI",  0, IND   , 6, 1, OTHER,    op_PEI},
   /* D5 */   { "CMP",  0, ZPX   , 4, 0, READOP,   op_CMP},
   /* D6 */   { "DEC",  0, ZPX   , 6, 0, RMWOP,    op_DEC},
   /* D7 */   { "CMP" , 0, IDLY  , 6, 1, READOP,   op_CMP},
   /* D8 */   { "CLD",  0, IMP   , 2, 0, OTHER,    op_CLD},
   /* D9 */   { "CMP",  0, ABSY  , 4, 0, READOP,   op_CMP},
   /* DA */   { "PHX",  0, IMP   , 3, 0, OTHER,    op_PHX},
   /* DB */   { "STP",  0, IMP   , 1, 1, OTHER,    0},        // WD65C02=3
   /* DC */   { "JML",  0, IAL   , 6, 1, OTHER,    0},
   /* DD */   { "CMP",  0, ABSX  , 4, 0, READOP,   op_CMP},
   /* DE */   { "DEC",  0, ABSX  , 7, 0, RMWOP,    op_DEC},
   /* DF */   { "CMP",  0, ALX   , 5, 1, READOP,   op_CMP},
   /* E0 */   { "CPX",  0, IMM   , 2, 0, OTHER,    op_CPX},
   /* E1 */   { "SBC",  0, INDX  , 6, 0, READOP,   op_SBC},
   /* E2 */   { "SEP",  0, IMM   , 3, 1, OTHER,    op_SEP},
   /* E3 */   { "SBC",  0, SR    , 4, 1, READOP,   op_SBC},
   /* E4 */   { "CPX",  0, ZP    , 3, 0, READOP,   op_CPX},
   /* E5 */   { "SBC",  0, ZP    , 3, 0, READOP,   op_SBC},
   /* E6 */   { "INC",  0, ZP    , 5, 0, RMWOP,    op_INC},
   /* E7 */   { "SBC",  0, IDL   , 6, 1, READOP,   op_SBC},
   /* E8 */   { "INX",  0, IMP   , 2, 0, OTHER,    op_INX},
   /* E9 */   { "SBC",  0, IMM   , 2, 0, OTHER,    op_SBC},
   /* EA */   { "NOP",  0, IMP   , 2, 0, OTHER,    0},
   /* EB */   { "XBA",  0, IMP   , 3, 1, OTHER,    op_XBA},
   /* EC */   { "CPX",  0, ABS   , 4, 0, READOP,   op_CPX},
   /* ED */   { "SBC",  0, ABS   , 4, 0, READOP,   op_SBC},
   /* EE */   { "INC",  0, ABS   , 6, 0, RMWOP,    op_INC},
   /* EF */   { "SBC",  0, ABL   , 5, 1, READOP,   op_SBC},
   /* F0 */   { "BEQ",  0, BRA   , 2, 0, BRANCHOP, op_BEQ},
   /* F1 */   { "SBC",  0, INDY  , 5, 0, READOP,   op_SBC},
   /* F2 */   { "SBC",  0, IND   , 5, 0, READOP,   op_SBC},
   /* F3 */   { "SBC",  0, ISY   , 7, 1, READOP,   op_SBC},
   /* F4 */   { "PEA",  0, ABS   , 5, 1, OTHER,    op_PEA},
   /* F5 */   { "SBC",  0, ZPX   , 4, 0, READOP,   op_SBC},
   /* F6 */   { "INC",  0, ZPX   , 6, 0, RMWOP,    op_INC},
   /* F7 */   { "SBC",  0, IDLY  , 6, 1, READOP,   op_SBC},
   /* F8 */   { "SED",  0, IMP   , 2, 0, OTHER,    op_SED},
   /* F9 */   { "SBC",  0, ABSY  , 4, 0, READOP,   op_SBC},
   /* FA */   { "PLX",  0, IMP   , 4, 0, OTHER,    op_PLX},
   /* FB */   { "XCE",  0, IMP   , 2, 1, OTHER,    op_XCE},
   /* FC */   { "JSR",  0, IND1X , 8, 1, OTHER,    op_JSR_new},
   /* FD */   { "SBC",  0, ABSX  , 4, 0, READOP,   op_SBC},
   /* FE */   { "INC",  0, ABSX  , 7, 0, RMWOP,    op_INC},
   /* FF */   { "SBC",  0, ALX   , 5, 1, READOP,   op_SBC}
};
