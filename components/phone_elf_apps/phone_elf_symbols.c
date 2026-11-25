/*
 * The symbols an ELF app can link against. The loader exports none of its own (CONFIG_ELF_LOADER_LIBC_SYMBOLS and
 * CONFIG_ELF_LOADER_ESPIDF_SYMBOLS are off): an ELF that needs anything not listed here is refused when it loads.
 *
 *   - memory, strings, ctype, stdlib helpers, math, printf to the console: the plain libc functions
 *   - files: fopen / remove / rename are sandboxed by the host (the data folder of the app with PERM_FILES, the folder of the
 *     package read only); fclose fread fwrite fseek ftell ... work on the FILE * those return
 *   - the helpers the compiler calls for 64-bit integers and double precision floats (libgcc)
 *
 * Add a name here when an app needs it; never export anything that reaches the network, the flash, NVS, a reset or another
 * app's files (those go through phone_elf_api_t, with a permission).
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "esp_log.h"
#include "esp_elf.h"
#include "phone_elf_api.h"
#include "phone_elf_host.h"

static const char *TAG = "ELFSYM";

/* ---- sandboxed files ---- */
static bool under(const char *path, const char *dir)
{
    const size_t n = strlen(dir);
    return n > 0 && path && strncmp(path, dir, n) == 0 && path[n] == '/' && !strstr(path, "..");
}

static FILE *sb_fopen(const char *path, const char *mode)
{
    const bool write = mode && strpbrk(mode, "wa+") != NULL;
    const unsigned perms = phone_elf_host_current_perms();
    if (path && (perms & PHONE_ELF_PERM_FILES) && under(path, phone_elf_host_current_data_dir())) {
        return fopen(path, mode);
    }
    if (path && !write && under(path, phone_elf_host_current_pkg_dir())) {
        return fopen(path, mode);       /* the package's own files, read only */
    }
    ESP_LOGW(TAG, "[%s] fopen(%s, %s) refused", phone_elf_host_current_id(), path ? path : "(null)", mode ? mode : "(null)");
    return NULL;
}

static int sb_remove(const char *path)
{
    if (path && (phone_elf_host_current_perms() & PHONE_ELF_PERM_FILES) && under(path, phone_elf_host_current_data_dir())) {
        return remove(path);
    }
    ESP_LOGW(TAG, "[%s] remove(%s) refused", phone_elf_host_current_id(), path ? path : "(null)");
    return -1;
}

static int sb_rename(const char *from, const char *to)
{
    const char *dir = phone_elf_host_current_data_dir();
    if ((phone_elf_host_current_perms() & PHONE_ELF_PERM_FILES) && under(from, dir) && under(to, dir)) {
        return rename(from, to);
    }
    ESP_LOGW(TAG, "[%s] rename refused", phone_elf_host_current_id());
    return -1;
}

/* libgcc: 64-bit integer and double precision helpers (the P4 has a single precision FPU only) */
#define LIBGCC(name) extern void name(void)
LIBGCC(__divdi3); LIBGCC(__udivdi3); LIBGCC(__moddi3); LIBGCC(__umoddi3); LIBGCC(__muldi3);
LIBGCC(__adddf3); LIBGCC(__subdf3); LIBGCC(__muldf3); LIBGCC(__divdf3);
LIBGCC(__floatsidf); LIBGCC(__floatunsidf); LIBGCC(__floatdidf); LIBGCC(__floatundidf);
LIBGCC(__fixdfsi); LIBGCC(__fixunsdfsi); LIBGCC(__fixdfdi); LIBGCC(__fixunsdfdi);
LIBGCC(__extendsfdf2); LIBGCC(__truncdfsf2);
LIBGCC(__eqdf2); LIBGCC(__nedf2); LIBGCC(__ltdf2); LIBGCC(__ledf2); LIBGCC(__gtdf2); LIBGCC(__gedf2); LIBGCC(__unorddf2);

static const struct esp_elfsym s_symbols[] = {
    /* memory */
    ESP_ELFSYM_EXPORT(malloc), ESP_ELFSYM_EXPORT(calloc), ESP_ELFSYM_EXPORT(realloc), ESP_ELFSYM_EXPORT(free),
    ESP_ELFSYM_EXPORT(memcpy), ESP_ELFSYM_EXPORT(memmove), ESP_ELFSYM_EXPORT(memset), ESP_ELFSYM_EXPORT(memcmp), ESP_ELFSYM_EXPORT(memchr),
    /* strings */
    ESP_ELFSYM_EXPORT(strlen), ESP_ELFSYM_EXPORT(strnlen), ESP_ELFSYM_EXPORT(strcmp), ESP_ELFSYM_EXPORT(strncmp),
    ESP_ELFSYM_EXPORT(strcasecmp), ESP_ELFSYM_EXPORT(strncasecmp), ESP_ELFSYM_EXPORT(strcpy), ESP_ELFSYM_EXPORT(strncpy),
    ESP_ELFSYM_EXPORT(strcat), ESP_ELFSYM_EXPORT(strncat), ESP_ELFSYM_EXPORT(strchr), ESP_ELFSYM_EXPORT(strrchr),
    ESP_ELFSYM_EXPORT(strstr), ESP_ELFSYM_EXPORT(strspn), ESP_ELFSYM_EXPORT(strcspn), ESP_ELFSYM_EXPORT(strpbrk),
    ESP_ELFSYM_EXPORT(strtol), ESP_ELFSYM_EXPORT(strtoul), ESP_ELFSYM_EXPORT(strtod), ESP_ELFSYM_EXPORT(strtof),
    ESP_ELFSYM_EXPORT(atoi), ESP_ELFSYM_EXPORT(atol), ESP_ELFSYM_EXPORT(atof),
    /* ctype */
    ESP_ELFSYM_EXPORT(isalpha), ESP_ELFSYM_EXPORT(isdigit), ESP_ELFSYM_EXPORT(isalnum), ESP_ELFSYM_EXPORT(isspace),
    ESP_ELFSYM_EXPORT(isupper), ESP_ELFSYM_EXPORT(islower), ESP_ELFSYM_EXPORT(isxdigit), ESP_ELFSYM_EXPORT(ispunct),
    ESP_ELFSYM_EXPORT(isprint), ESP_ELFSYM_EXPORT(toupper), ESP_ELFSYM_EXPORT(tolower),
    /* stdlib */
    ESP_ELFSYM_EXPORT(abs), ESP_ELFSYM_EXPORT(labs), ESP_ELFSYM_EXPORT(rand), ESP_ELFSYM_EXPORT(srand),
    ESP_ELFSYM_EXPORT(qsort), ESP_ELFSYM_EXPORT(bsearch),
    /* stdio: the console and strings; files below */
    ESP_ELFSYM_EXPORT(printf), ESP_ELFSYM_EXPORT(puts), ESP_ELFSYM_EXPORT(putchar), ESP_ELFSYM_EXPORT(snprintf),
    ESP_ELFSYM_EXPORT(vsnprintf), ESP_ELFSYM_EXPORT(sscanf),
    /* files (FILE * from the sandboxed fopen) */
    ESP_ELFSYM_EXPORT_NAMED("fopen", sb_fopen), ESP_ELFSYM_EXPORT_NAMED("remove", sb_remove), ESP_ELFSYM_EXPORT_NAMED("rename", sb_rename),
    ESP_ELFSYM_EXPORT(fclose), ESP_ELFSYM_EXPORT(fread), ESP_ELFSYM_EXPORT(fwrite), ESP_ELFSYM_EXPORT(fseek), ESP_ELFSYM_EXPORT(ftell),
    ESP_ELFSYM_EXPORT(fflush), ESP_ELFSYM_EXPORT(fgets), ESP_ELFSYM_EXPORT(fputs), ESP_ELFSYM_EXPORT(feof), ESP_ELFSYM_EXPORT(ferror),
    ESP_ELFSYM_EXPORT(rewind), ESP_ELFSYM_EXPORT(fprintf), ESP_ELFSYM_EXPORT(fscanf),
    /* math, single and double precision */
    ESP_ELFSYM_EXPORT(sinf), ESP_ELFSYM_EXPORT(cosf), ESP_ELFSYM_EXPORT(tanf), ESP_ELFSYM_EXPORT(atanf), ESP_ELFSYM_EXPORT(atan2f),
    ESP_ELFSYM_EXPORT(sqrtf), ESP_ELFSYM_EXPORT(powf), ESP_ELFSYM_EXPORT(expf), ESP_ELFSYM_EXPORT(logf), ESP_ELFSYM_EXPORT(fabsf),
    ESP_ELFSYM_EXPORT(floorf), ESP_ELFSYM_EXPORT(ceilf), ESP_ELFSYM_EXPORT(roundf), ESP_ELFSYM_EXPORT(fmodf),
    ESP_ELFSYM_EXPORT(sin), ESP_ELFSYM_EXPORT(cos), ESP_ELFSYM_EXPORT(tan), ESP_ELFSYM_EXPORT(atan), ESP_ELFSYM_EXPORT(atan2),
    ESP_ELFSYM_EXPORT(sqrt), ESP_ELFSYM_EXPORT(pow), ESP_ELFSYM_EXPORT(exp), ESP_ELFSYM_EXPORT(log), ESP_ELFSYM_EXPORT(log10),
    ESP_ELFSYM_EXPORT(fabs), ESP_ELFSYM_EXPORT(floor), ESP_ELFSYM_EXPORT(ceil), ESP_ELFSYM_EXPORT(round), ESP_ELFSYM_EXPORT(fmod),
    /* libgcc */
    ESP_ELFSYM_EXPORT(__divdi3), ESP_ELFSYM_EXPORT(__udivdi3), ESP_ELFSYM_EXPORT(__moddi3), ESP_ELFSYM_EXPORT(__umoddi3),
    ESP_ELFSYM_EXPORT(__muldi3), ESP_ELFSYM_EXPORT(__adddf3), ESP_ELFSYM_EXPORT(__subdf3), ESP_ELFSYM_EXPORT(__muldf3),
    ESP_ELFSYM_EXPORT(__divdf3), ESP_ELFSYM_EXPORT(__floatsidf), ESP_ELFSYM_EXPORT(__floatunsidf), ESP_ELFSYM_EXPORT(__floatdidf),
    ESP_ELFSYM_EXPORT(__floatundidf), ESP_ELFSYM_EXPORT(__fixdfsi), ESP_ELFSYM_EXPORT(__fixunsdfsi), ESP_ELFSYM_EXPORT(__fixdfdi),
    ESP_ELFSYM_EXPORT(__fixunsdfdi), ESP_ELFSYM_EXPORT(__extendsfdf2), ESP_ELFSYM_EXPORT(__truncdfsf2),
    ESP_ELFSYM_EXPORT(__eqdf2), ESP_ELFSYM_EXPORT(__nedf2), ESP_ELFSYM_EXPORT(__ltdf2), ESP_ELFSYM_EXPORT(__ledf2),
    ESP_ELFSYM_EXPORT(__gtdf2), ESP_ELFSYM_EXPORT(__gedf2), ESP_ELFSYM_EXPORT(__unorddf2),
    ESP_ELFSYM_END,
};

void phone_elf_symbols_register(void)
{
    ESP_LOGI(TAG, "register the symbols of the apps: %d", esp_elf_register_symbol(s_symbols));
}
