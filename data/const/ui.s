	.include "asm/macros.inc"
	.include "constants/constants.inc"

	.section .rodata
    .align 2
@ before: playerItemCellOverwriteSet
@ in here: ui.c, color.c, common.c
@ afterwards on data side: sineTable.c
@ after: octorok.c


@ ui.c const

gUnk_080C8F2C:: @ 080C8F2C
	.incbin "data_080B7B74/gUnk_080C8F2C.bin"

gUnk_080C8F54:: @ 080C8F54
	.incbin "data_080B7B74/gUnk_080C8F54.bin"

@ TODO this data is accessed using DMA in DrawChargeBar
gUnk_080C8F7C:: @ 080C8F7C
	.4byte gUnk_data0_085C4DA0
	.4byte gUnk_data1_085C4E60
	.4byte gUnk_data2_085C4F20
	.4byte gUnk_data3_085C4FE0


gUIElementDefinitions:: @ 080C8F8C
	.incbin "data_080B7B74/gUIElementDefinitions_1_USA-JP-DEMO_USA-DEMO_JP.bin"
	.4byte ButtonUIElement
	.incbin "data_080B7B74/gUIElementDefinitions_3_USA-JP-DEMO_USA-DEMO_JP.bin"
	.4byte ButtonUIElement
	.incbin "data_080B7B74/gUIElementDefinitions_5_USA-JP-DEMO_USA-DEMO_JP.bin"
	.4byte ButtonUIElement
	.incbin "data_080B7B74/gUIElementDefinitions_7_USA-JP-DEMO_USA-DEMO_JP.bin"
	.4byte ItemUIElement
	.incbin "data_080B7B74/gUIElementDefinitions_9_USA-JP-DEMO_USA-DEMO_JP.bin"
	.4byte ItemUIElement
	.incbin "data_080B7B74/gUIElementDefinitions_11_USA-JP-DEMO_USA-DEMO_JP.bin"
	.4byte TextUIElement
	.incbin "data_080B7B74/gUIElementDefinitions_13_USA-JP-DEMO_USA-DEMO_JP.bin"
	.4byte HeartUIElement
	.incbin "data_080B7B74/gUIElementDefinitions_15_USA-JP-DEMO_USA-DEMO_JP.bin"
	.4byte EzloNagUIElement
	.incbin "data_080B7B74/gUIElementDefinitions_17_USA-JP-DEMO_USA-DEMO_JP.bin"
	.4byte EzloNagUIElement
	.incbin "data_080B7B74/gUIElementDefinitions_19_USA-JP-DEMO_USA-DEMO_JP.bin"
	.4byte TextUIElement
	.incbin "data_080B7B74/gUIElementDefinitions_21_USA-JP-DEMO_USA-DEMO_JP.bin"
	.4byte TextUIElement
	.incbin "data_080B7B74/gUIElementDefinitions_22.bin"

ButtonUIElement_Actions:: @ 080C903C
	.4byte ButtonUIElement_Action0
	.4byte ButtonUIElement_Action1

gUnk_080C9044:: @ 080C9044
	.incbin "data_080B7B74/gUnk_080C9044.bin"

EzloNagUIElement_Actions:: @ 080C904C
	.4byte EzloNagUIElement_Action0
	.4byte EzloNagUIElement_Action1
	.4byte EzloNagUIElement_Action2

gUnk_080C9058:: @ 080C9058
	.incbin "data_080B7B74/gUnk_080C9058.bin"

@ ui.c
gUnk_080C9094:: @ 080C9094
	.incbin "data_080B7B74/gUnk_080C9094.bin"

