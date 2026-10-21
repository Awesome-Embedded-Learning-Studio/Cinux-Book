.section .text
.global StoreGdtSnapshot
.type StoreGdtSnapshot, @function
StoreGdtSnapshot:
    sgdt (%rdi)
    ret
.size StoreGdtSnapshot, . - StoreGdtSnapshot
.section .note.GNU-stack,"",@progbits
