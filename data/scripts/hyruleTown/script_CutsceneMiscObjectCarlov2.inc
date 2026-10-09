@ Hyrule Town CUTSCENE_ORCHESTRATOR at Carlov
SCRIPT_START script_CutsceneMiscObjectCarlov2
	BeginBlock
	DoPostScriptAction 0x0008
	SetAnimationState 0x0004
	DoPostScriptAction 0x0000
	EndBlock
script_080145F8:
script_08014632:
	CheckRoomFlag 0x0000
	JumpIfNot script_08014632
	SetPlayerIdle
	Call DisablePauseMenuAndPutAwayItems
script_08014642:
	CheckRoomFlag 0x0004
	JumpIf script_08014642
	SetSyncFlag 0x00000004
	WaitForSyncFlagAndClear 0x00000008
	Call EnablePauseMenu
	EnablePlayerControl
	Jump script_080145F8
	.align 2, 0

