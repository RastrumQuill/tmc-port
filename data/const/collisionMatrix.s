	.include "asm/macros.inc"
	.include "constants/constants.inc"

	.section .rodata
    .align 2
@ before: playerItemCellOverwriteSet

@ collision.c
gCollisionMtx:: @ 080B7B74
	.incbin "data_080B7B74/gCollisionMtx.bin"
	.incbin "data_080B7B74/gUnk_080B802E.bin"
	.incbin "data_080B7B74/gUnk_080BA2C0.bin"
	.incbin "data_080B7B74/gUnk_080B7B74_2_USA-JP-DEMO_USA-DEMO_JP.bin"
	.incbin "data_080B7B74/gUnk_080B7B74_3.bin"
	.incbin "data_080B7B74/gUnk_080B7B74_6_USA-DEMO_USA-DEMO_JP.bin"
	.incbin "data_080B7B74/gUnk_080B7B74_8_USA-JP-DEMO_USA-DEMO_JP.bin"
	.incbin "data_080B7B74/gUnk_080B7B74_9.bin"
