/* 
 * LC-3 VM implementation (from https://justinmeiners.github.io/lc3-vm/)
 * spec: https://justinmeiners.github.io/lc3-vm/supplies/lc3-isa.pdf
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/termios.h>
#include <sys/mman.h>

/*
 * Registers
 *
 * 8 general purpose registers (R0-R7)
 * 1 program counter (PC) register
 * 1 condition flags (COND) register
 */
enum
{
	R_R0 = 0,
	R_R1,
	R_R2,
	R_R3,
	R_R4,
	R_R5,
	R_R6,
	R_R7,
	R_PC,
	R_COND,
	R_COUNT
};

/*
 * Opcodes for the instruction set
 */
enum
{
	OP_BR = 0,			/* branch */
	OP_ADD,				/* add */
	OP_LD,				/* load */
	OP_ST,				/* store */
	OP_JSR,				/* jump register */
	OP_AND,				/* bitwise and */
	OP_LDR,				/* load register */
	OP_STR,				/* store register */
	OP_RTI,				/* unused */
	OP_NOT,				/* bitwise not */
	OP_LDI,				/* load indirect */
	OP_STI,				/* store indirect */
	OP_JMP,				/* jump */
	OP_RES,				/* reserved (unused) */
	OP_LEA,				/* load effective address */
	OP_TRAP				/* execute trap */
};

/*
 * Condition flags
 */
enum
{
	FL_POS = 1 << 0,	/* positive */
	FL_ZRO = 1 << 1,	/* zero */
	FL_NEG = 1 << 2,	/* negative */
};

/*
 * Memory mapped registers
 */
enum
{
	MR_KBSR = 0xFE00,	/* keyboard status */
	MR_KBDR = 0xFE02	/* keyboard data */
};

/* 
 * Trap codes
 */
enum
{
	TRAP_GETC = 0x20,	/* get character from keyboard, not echoed onto the terminal */
	TRAP_OUT = 0x21,	/* output a character */
	TRAP_PUTS = 0x22,	/* output a word string */
	TRAP_IN = 0x23,		/* get character from keyboard, echoed onto the terminal */
	TRAP_PUTSP = 0x24,	/* output a byte string */
	TRAP_HALT = 0x25	/* halt the program */
};

/* 
 * LC-3 has 65,536 memory locations (16-bit unsigned)
 * Memory will be stored in an array.
 */
uint16_t memory[UINT16_MAX];

/* Store registers in an array */
uint16_t reg[R_COUNT];

/* 
 * Sign extend helper 
 */
uint16_t sign_extend(uint16_t x, int bit_count)
{
	if ((x >> (bit_count - 1)) & 1) {
		x |= (0xFFFF << bit_count);
	}
	return x;
}

/*
 * Swap helper
 */
uint16_t swap16(uint16_t x)
{
	return (x << 8) | (x >> 8);
}

/* 
 * Update condition flag
 */
void update_flags(uint16_t r)
{
	if (reg[r] == 0)
	{
		reg[R_COND] = FL_ZRO;
	}
	else if (reg[r] >> 15) /* 1 in left-most bit indicates neg */
	{
		reg[R_COND] = FL_NEG;
	}
	else
	{
		reg[R_COND] = FL_POS;
	}
}

/*
 * Read image file
 */
void read_image_file(FILE* file)
{
	/* the origin tells us where in memory to place the image */
	uint16_t origin;
	fread(&origin, sizeof(origin), 1, file);
	origin = swap16(origin);

	/* we know the maximum file size so we only need one fread */
	uint16_t max_read = UINT16_MAX - origin;
	uint16_t* p = memory + origin;
	size_t read = fread(p, sizeof(uint16_t), max_read, file);

	/* swap to little endian */
	while (read-- > 0)
	{
		*p = swap16(*p);
		++p;
	}
}

/*
 * Read image
 */
int read_image(const char* image_path)
{
	FILE* file = fopen(image_path, "rb");
	if (!file) { return 0; };
	read_image_file(file);
	fclose(file);
	return 1;
}

/*
 * Check key helper
 */
uint16_t check_key()
{
	fd_set readfds;
	FD_ZERO(&readfds);
	FD_SET(STDIN_FILENO, &readfds);

	struct timeval timeout;
	timeout.tv_sec = 0;
	timeout.tv_usec = 0;
	return select(1, &readfds, NULL, NULL, &timeout) != 0;
}

/*
 * Write to memory
 */
void mem_write(uint16_t address, uint16_t val)
{
	memory[address] = val;
}

/*
 * Read from memory
 */
uint16_t mem_read(uint16_t address)
{
	if (address == MR_KBSR)
	{
		if (check_key())
		{
			memory[MR_KBSR] = (1 << 15);
			memory[MR_KBDR] = getchar();
		}
		else
		{
			memory[MR_KBSR] = 0;
		}
	}
	return memory[address];
}

/*
 * Input buffering
 */
struct termios original_tio;

/*
 * Disable input buff
 */
void disable_input_buffering()
{
	tcgetattr(STDIN_FILENO, &original_tio);
	struct termios new_tio = original_tio;
	new_tio.c_lflag &= ~ICANON & ~ECHO;
	tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
}

/*
 * Restore input buff
 */
void restore_input_buffering()
{
	tcsetattr(STDIN_FILENO, TCSANOW, &original_tio);
}

/*
 * handle interrupt sig
 */
void handle_interrupt(int signal)
{
	restore_input_buffering();
	printf("\n");
	exit(-2);
}

/*
 * Main loop
 */
int main(int argc, const char* argv[])
{
	if (argc < 2)
	{
		printf("lc3 [image-file1] ...\n");
		exit(2);
	}

	for (int j = 1; j < argc; ++j)
	{
		if (!read_image(argv[j]))
		{
			printf("failed to load image: %s\n", argv[j]);
			exit(1);
		}
	}

	signal(SIGINT, handle_interrupt);
	disable_input_buffering();

	/* 
	 * set the PC (program counter) to starting position
	 * 0x3000 is default
	 */
	enum { PC_START = 0x3000 };
	reg[R_PC] = PC_START;

	int running = 1;
	while (running)
	{
		/* Increment and fetch PC */
		uint16_t instr = mem_read(reg[R_PC]++);
		uint16_t op = instr >> 12;

		switch (op)
		{
			case OP_ADD:
				{
					/* destination register (DR) */
					uint16_t r0 = (instr >> 9) & 0x7;
					/* first operand (SR1) */
					uint16_t r1 = (instr >> 6) & 0x7;
					/* whether we're in immediate mode */
					uint16_t imm_flag = (instr >> 5) & 0x1;

					if (imm_flag)
					{
						/* if immediate mode, get sign-extended value */
						uint16_t imm5 = sign_extend(instr & 0x1F, 5);
						/* perform operation with value and assign to destination */
						reg[r0] = reg[r1] + imm5;
					}
					else
					{
						uint16_t r2 = instr & 0x7;
						reg[r0] = reg[r1] + reg[r2];
					}

					/* update flags accordingly */
					update_flags(r0);
				}

				break;
			case OP_AND:
				{
					/* DR */
					uint16_t r0 = (instr >> 9) & 0x7;
					/* SR1 */
					uint16_t r1 = (instr >> 6) & 0x7;
					uint16_t imm_flag = (instr >> 5) & 0x1;

					if (imm_flag)
					{
						uint16_t imm5 = sign_extend(instr & 0x1F, 5);
						reg[r0] = reg[r1] & imm5;
					}
					else
					{
						uint16_t r2 = instr & 0x7;
						reg[r0] = reg[r1] & reg[r2];
					}

					update_flags(r0);
				}

				break;
			case OP_NOT:
				{
					uint16_t r0 = (instr >> 9) & 0x7;
					uint16_t r1 = (instr >> 6) & 0x7;
					/* '~' is bitwise NOT (complement) */
					reg[r0] = ~reg[r1];

					update_flags(r0);
				}

				break;
			case OP_BR:
				{
					/* Branch tests condition codes against the current condition flags
					 * to determine whether to branch to a location
					 */

					/* PCoffset 9 */
					uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
					uint16_t cond = (instr >> 9) & 0x7;

					if (cond & reg[R_COND])
					{
						reg[R_PC] += pc_offset;
					}
				}

				break;
			case OP_JMP:
				{
					/* 
					 * Jump also handles RET (return), since RET just 
					 * happens when R1 is 7 (111)
					 */
					uint16_t r1 = (instr >> 6) & 0x7;
					reg[R_PC] = reg[r1];
				}

				break;
			case OP_JSR:
				{
					/* 
					 * JSR or JSRR: Jump to subroutine
					 */
					uint16_t r1 = (instr >> 6) & 0x7;
					uint16_t pc_offset_long = sign_extend(instr & 0x7FF, 11);
					uint16_t flag = (instr >> 11) & 1;

					reg[R_R7] = reg[R_PC];
					if (flag)
					{
						reg[R_PC] += pc_offset_long; /* JSR */
					}
					else
					{
						reg[R_PC] = reg[r1]; /* JSRR */
					}
				}

				break;
			case OP_LD:
				{
					uint16_t r0 = (instr >> 9) & 0x7;
					/* PCoffset 9 */
					uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);

					reg[r0] = mem_read(reg[R_PC] + pc_offset);

					update_flags(r0);
				}

				break;
			case OP_LDI:
				{
					uint16_t r0 = (instr >> 9) & 0x7;
					uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
					/* pc_offset + current PC = location to get address for destination value */
					reg[r0] = mem_read(mem_read(reg[R_PC] + pc_offset));

					update_flags(r0);
				}
				break;
			case OP_LDR:
				{
					uint16_t r0 = (instr >> 9) & 0x7;
					uint16_t r1 = (instr >> 6) & 0x7;
					uint16_t pc_offset_short = sign_extend(instr & 0x3F, 6);

					reg[r0] = mem_read(reg[r1] + pc_offset_short);

					update_flags(r0);
				}

				break;
			case OP_LEA:
				{
					uint16_t r0 = (instr >> 9) & 0x7;
					uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);

					reg[r0] = reg[R_PC] + pc_offset;

					update_flags(r0);
				}

				break;
			case OP_ST:
				{
					uint16_t r0 = (instr >> 9) & 0x7;
					uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);

					mem_write(reg[R_PC] + pc_offset, reg[r0]);
				}

				break;
			case OP_STI:
				{
					uint16_t r0 = (instr >> 9) & 0x7;
					uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);

					mem_write(mem_read(reg[R_PC] + pc_offset), reg[r0]);
				}

				break;
			case OP_STR:
				{
					uint16_t r0 = (instr >> 9) & 0x7;
					uint16_t r1 = (instr >> 6) & 0x7;
					uint16_t pc_offset_short = sign_extend(instr & 0x3F, 6);

					mem_write(reg[r1] + pc_offset_short, reg[r0]);
				}

				break;
			case OP_TRAP:
				{
					uint16_t trap_code = instr & 0xFF;

					reg[R_R7] = reg[R_PC];
					switch (trap_code)
					{
						case TRAP_GETC:
							reg[R_R0] = (uint16_t)getchar();

							break;
						case TRAP_OUT:
							{
								putc((char)reg[R_R0], stdout);

								fflush(stdout);
							}

							break;
						case TRAP_PUTS:
							{
								uint16_t* c = memory + reg[R_R0];

								while (*c)
								{
									putc((char)*c, stdout);
									++c;
								}

								fflush(stdout);
							}

							break;
						case TRAP_IN:
							{
								printf("Enter a character: ");
								char c = getchar();

								putc(c, stdout);

								reg[R_R0] = (uint16_t)c;
							}

							break;
						case TRAP_PUTSP:
							{
								uint16_t* c = memory + reg[R_R0];

								while (*c)
								{
									char char1 = (*c) & 0xFF;
									putc(char1, stdout);
									char char2 = (*c) >> 8;
									if (char2) putc(char2, stdout);
									++c;
								}

								fflush(stdout);
							}

							break;
						case TRAP_HALT:
							{
								puts("HALT");
								fflush(stdout);
								running = 0;
							}

							break;
					}
				}

				break;
			case OP_RES:
			case OP_RTI:
			default:
				/* unused, bad opcode */
				abort();
				break;
		}
	}

	restore_input_buffering();
}

