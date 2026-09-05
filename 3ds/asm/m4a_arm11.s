@ Runs the original m4a mixer in place on the ARM11, instead of relocating it.
@
@ SoundMain (src/m4a_1.s) finishes with a tail jump:
@
@     ldr r3, lt_SoundMainRAM_Buffer
@     bx  r3
@     ...
@     lt_SoundMainRAM_Buffer: .word SoundMainRAM_Buffer + 1
@
@ On a GBA that lands in IWRAM, where m4aSoundInit has copied SoundMainRAM to,
@ because IWRAM is zero-wait-state and the mixer is the hot loop.
@
@ Here it cannot. IWRAM on this port is an offset into gGbaMem (3ds/gba_mem.c),
@ which is an ordinary .bss array in a read/write segment, and a CXI has no
@ read/write/execute segment to put it in. Jumping there faults.
@
@ There is also no reason to: the ARM11 runs .text at full speed and there is no
@ wait-state penalty to escape. So the buffer symbol simply IS the routine, the
@ jump lands on the original code where it already sits, and src/m4a.c skips the
@ copy that would otherwise write to read-only memory.
@
@ Declared %object rather than %function deliberately. For a Thumb %function the
@ toolchain sets the low bit itself, and the `+ 1` in that literal would then be
@ added on top of it and jump one halfword past the entry point.

	.section .text.m4a_arm11, "ax", %progbits

	.global SoundMainRAM_Buffer
	.type   SoundMainRAM_Buffer, %object
	.set    SoundMainRAM_Buffer, SoundMainRAM
