@ Hyrule Town CUTSCENE_ORCHESTRATOR at Carlov 2
SCRIPT_START script_CutsceneMiscObjectCarlov
	BeginBlock
	SetEntityPositionRelative 0x01f8, 0x00e0
	CheckLocalFlag 0x005e
	JumpIf script_080145E4
	EndBlock
script_080145C2:
	CheckRoomFlag 0x0005
	JumpIfNot script_080145C2
	SetPlayerIdle
	ClearRoomFlag 0x0005
	Wait 0x000c
	FaceAwayFromPlayer
	MessageFromTargetPos TEXT_CARLOV, 0x2e, 0x000c
	WaitUntilTextboxCloses
	EnablePlayerControl
	Jump script_080145C2
script_080145E4:
	EndBlock
SCRIPT_END
