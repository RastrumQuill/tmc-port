@ DHC CUTSCENE_ORCHESTRATOR Towards Vaati in room with Ball&Chain soldier
SCRIPT_START script_CutsceneOrchestratorBallAndChainSoldierRoom
	BeginBlock
	Call SetPriorityPlayerEvent
	EndBlock
	CheckLocalFlag 0x007c
	JumpIf script_0800E4FA
script_0800E4B6:
	CheckPlayerInRegion 0x0088, 0x00f8, 0x0820
	JumpIfNot script_0800E4B6
	BeginBlock
	SetPlayerIdle
	EndBlock
	Call CreatePlayerExclamationMark
	Wait 0x003c
	MessageFromTarget TEXT_VAATI2, 0x0c
	WaitUntilTextboxCloses
	Wait 0x001e
	MessageFromTarget TEXT_VAATI2, 0x0d
	WaitUntilTextboxCloses
	Wait 0x001e
	EzloMessage TEXT_VAATI2, 0x0e
	Wait 0x003c
	WaitUntilTextboxCloses
	BeginBlock
	PlayBgm BGM_FIGHT_THEME2
	SetLocalFlag 0x007c
script_0800E4FA:
	EnablePlayerControl
	DoPostScriptAction 0x0006
	EndBlock
SCRIPT_END
