@ Sends the original mixer's tail jump to the mixer itself.
@
@ SoundMain (src/m4a_1.s) ends with:
@
@     ldr r3, lt_SoundMainRAM_Buffer
@     bx  r3
@     ...
@     lt_SoundMainRAM_Buffer: .word SoundMainRAM_Buffer + 1
@
@ On a GBA, that buffer is an IWRAM copy that m4aSoundInit makes, because IWRAM
@ has zero wait states and the mixer is the hot loop. That is not possible
@ here. IWRAM is an offset into gGbaMem (3ds/gba_mem.c), a usual .bss array,
@ and a CXI has no read/write/execute segment for code. Also, a copy gives no
@ speed, because the ARM11 runs .text at full speed.
@
@ Thus SoundMainRAM_Buffer is two instructions here that jump to the real
@ routine. Also, src/m4a.c does not make the copy, which would write to
@ read-only memory.
@
@ Two important details:
@
@ - No .thumb_func, and no .type. For a Thumb %function, the toolchain sets
@   bit 0 of the symbol itself. The `+ 1` in that literal would then add one
@   more, and point one halfword past the entry. A plain NOTYPE label at an
@   even address makes the `+ 1` the Thumb bit, as for the real buffer.
@
@ - A trampoline, not an alias. The `.set` directive cannot name a symbol from
@   a different object. The linker evaluates `ld --defsym` before symbol
@   resolution, so `--defsym SoundMainRAM_Buffer=SoundMainRAM` gives 0 with no
@   link error, and the tail jump faults at address 0.
@
@ SoundMain loaded the target into r3, so this code can overwrite r3. The
@ registers r0-r2 and r4-r7 hold the arguments of the mixer, and do not change.

	.section .text.m4a_arm11, "ax", %progbits
	.thumb
	.align 2
	.global SoundMainRAM_Buffer
SoundMainRAM_Buffer:
	ldr r3, .L_SoundMainRAM
	bx  r3

	.align 2
.L_SoundMainRAM:
	.word SoundMainRAM
