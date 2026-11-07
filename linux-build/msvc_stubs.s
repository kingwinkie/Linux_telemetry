.global "??3@YAXPAX@Z"
"??3@YAXPAX@Z":
    subl $12, %esp
    movl 16(%esp), %eax
    movl %eax, (%esp)
    call _free
    addl $12, %esp
    ret
