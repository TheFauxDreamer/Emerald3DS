@ Absolute symbols the m4a engine reads via ((u16)gNumMusicPlayers) / ((u32)gMaxLines).
@ Their *address* is the value. data/sound_data.s in this fork omits the footer
@ that defines them, so provide them here for the RP2350 archive.
	.global gNumMusicPlayers
	.global gMaxLines
	.set gNumMusicPlayers, 4   @ BGM, SE1, SE2, SE3
	.set gMaxLines, 0
