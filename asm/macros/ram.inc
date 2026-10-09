	.ifndef GUARD_ASM_MACROS_RAM_INC
	.set GUARD_ASM_MACROS_RAM_INC, 1


    .set RAM_DIFF, 0x050AC28C


    .macro ram_addr x
    .4byte \x - RAM_DIFF
    .endm

	.endif @ GUARD_ASM_MACROS_RAM_INC
