/* chimeraMTD: an overlapping copy to a higher address used to run
 * "std; rep movsb", one byte at a time - backward string moves get none of
 * the processor's fast-string help. PCSX2's recompilers make a hole in a
 * sorted block array that way on every new block, and it cost God Hand about
 * 5% of the machine's thread. Now it moves 16 bytes at a time from the end:
 * each chunk is read before anything below it is written, and the first 16
 * bytes are read up front and stored last, so the bytes that arrive are the
 * bytes a byte-at-a-time copy would have written. */
.global memmove
.type memmove,@function
memmove:
	mov %rdi,%rax
	sub %rsi,%rax
	cmp %rdx,%rax
.hidden __memcpy_fwd
	jae __memcpy_fwd
	mov %rdi,%rax
	cmp $16,%rdx
	jb 3f
	movdqu (%rsi),%xmm1
	lea -16(%rdx),%rcx
1:	movdqu (%rsi,%rcx),%xmm0
	movdqu %xmm0,(%rdi,%rcx)
	sub $16,%rcx
	ja 1b
	movdqu %xmm1,(%rdi)
	ret
3:	test %rdx,%rdx
	jz 4f
2:	movzbl -1(%rsi,%rdx),%ecx
	mov %cl,-1(%rdi,%rdx)
	dec %rdx
	jnz 2b
4:	ret
