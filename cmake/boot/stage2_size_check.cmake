file(SIZE "${SIZE_FILE}" bytes)
math(EXPR sectors "(${bytes} + 511) / 512")
if(sectors GREATER MAX_SECTORS)
    message(FATAL_ERROR
        "stage2.bin is ${bytes} bytes (${sectors} sectors), exceeds kStage2Sectors=${MAX_SECTORS} (${MAX_SECTORS} * 512 bytes); raise kStage2Sectors in boot/layout.hpp and the MBR DAP follows")
endif()
