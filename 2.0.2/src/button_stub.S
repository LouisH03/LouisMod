    .text
    .globl _button_found_stub
    .def _button_found_stub; .scl 2; .type 32; .endef

/*
 * Replays the six bytes replaced at TmForever.exe+0xC8344 while calling
 * on_button_found with the control pointer in EAX and its replay dialog in
 * EDI.
 */
_button_found_stub:
    movl %eax, %ebp
    pushfl
    pushal
    pushl %edi
    pushl %eax
    call _on_button_found
    addl $8, %esp
    popal
    popfl
    leal 0x5c(%esp), %ecx
    jmp *_g_button_return
