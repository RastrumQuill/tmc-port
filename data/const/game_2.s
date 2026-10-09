	.include "asm/macros.inc"
	.include "constants/constants.inc"

	.section .rodata
	.align 2


@ kinstone.c, worldEvent2.c
gUnk_080FEAC8:: @ 080FEAC8
	.incbin "data_080FC8A4/gUnk_080FEAC8.bin"
	.incbin "data_080FC8A4/gUnk_080FEAC8_4_USA-DEMO_USA.bin"

	.incbin "data_080FC8A4/gUnk_080FEAC8_8_USA-DEMO_USA.bin"

	.incbin "data_080FC8A4/gUnk_080FEAC8_9.bin"

@ kinstone.c
gUnk_080FEBE8:: @ 080FEBE8
	.incbin "data_080FC8A4/gUnk_080FEBE8.bin"

gUnk_080FEC28:: @ 080FEC28
	.incbin "data_080FC8A4/gUnk_080FEC28.bin"

gUnk_080FECC8:: @ 080FECC8
	.incbin "data_080FC8A4/gUnk_080FECC8.bin"

gUnk_080FED18:: @ 080FED18
	.incbin "data_080FC8A4/gUnk_080FED18.bin"

gUnk_080FED58:: @ 080FED58
	.incbin "data_080FC8A4/gUnk_080FED58.bin"

@ kinstone.c
gLilypadRails:: @ 080FED98
	.4byte gUnk_080FEDA4
	.4byte gUnk_080FEDDA
	.4byte gUnk_080FEDF8

gUnk_080FEDA4:: @ 080FEDA4
	.incbin "data_080FC8A4/gUnk_080FEDA4.bin"

gUnk_080FEDDA:: @ 080FEDDA
	.incbin "data_080FC8A4/gUnk_080FEDDA.bin"

gUnk_080FEDF8:: @ 080FEDF8
	.incbin "data_080FC8A4/gUnk_080FEDF8.bin"

gUnk_080FEE18:: @ 080FEE18
	.incbin "data_080FC8A4/gUnk_080FEE18.bin"

gUnk_080FEE38:: @ 080FEE38
	.incbin "data_080FC8A4/gUnk_080FEE38.bin"

gUnk_080FEE48:: @ 080FEE48
	.incbin "data_080FC8A4/gUnk_080FEE48.bin"

@ kinstone.c
gUnk_080FEE58:: @ 080FEE58
	.incbin "data_080FC8A4/gUnk_080FEE58_3_USA-DEMO_USA.bin"

@ kinstone.c
gUnk_080FEE78:: @ 080FEE78
	.incbin "data_080FC8A4/gUnk_080FEE78.bin"
