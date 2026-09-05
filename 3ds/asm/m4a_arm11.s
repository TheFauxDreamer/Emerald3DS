@ Sends the original mixer's tail jump to the mixer itself.
@
@ SoundMain (src/m4a_1.s) ends with:
@
@     ldr r3, lt_SoundMainRAM_Buffer
@     bx  r3
@     ...
@     lt_SoundMainRAM_Buffer: .word SoundMainRAM_Buffer + 1
@
@ On a GBA that buffer is an IWRAM copy m4aSoundInit makes, because IWRAM is
@ zero-wait-state and the mixer is the hot loop. Here it cannot be: IWRAM is an
@ offset into gGbaMem (3ds/gba_mem.c), an ordinary .bss array, and a CXI has no
@ read/write/execute segment to put code in. There is also nothing to gain,
@ since the ARM11 runs .text at full speed.
@
@ So SoundMainRAM_Buffer is two instructions here that jump to the real routine,
@ and src/m4a.c skips the copy that would otherwise write to read-only memory.
@
@ Two details, both of which have already gone wrong once:
@
@   NO .thumb_func, and no .type. For a Thumb %function the toolchain sets bit 0
@   of the symbol itself, and the `+ 1` in that literal would then be added on
@   top and land one halfword past the entry. A plain NOTYPE label at an even
@   address makes the `+ 1` the Thumb bit, exactly as it is for the real buffer.
@
@   A trampoline rather than an alias. `.set` cannot name a symbol from another
@   object, and `ld --defsym` evaluates its expression before symbol resolution,
@   so `--defsym SoundMainRAM_Buffer=SoundMainRAM` silently resolved to 0 and
@   the tail jump faulted at address 0 with no link error.
@
@ r3 is the register SoundMain loaded the target into, so clobbering it is free.
@ r0-r2 and r4-r7 carry the mixer's arguments and are left alone.

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
