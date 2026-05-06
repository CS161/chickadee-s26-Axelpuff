tui new-layout main {-horizontal asm 3 src 1} 1 {-horizontal regs 1 cmd 1} 1
layout main

# b syscall_entry
# b *0x10122e
# b resume_yieldstate
