#pragma once
/* ELF loader experiment: loads an ELF app into PSRAM and runs it (embedded copy, then /sdcard/appdata/elftest/hello.elf). */
#ifdef __cplusplus
extern "C" {
#endif
void phone_elf_test_start(void);
#ifdef __cplusplus
}
#endif
