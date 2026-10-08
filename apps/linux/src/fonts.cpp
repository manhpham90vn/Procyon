// The bundled typefaces (design/fonts, SIL OFL 1.1) linked into the executable, the counterpart of
// the RCDATA resources of the Windows app. PROCYON_FONT_DIR is set by CMakeLists.txt.
#ifndef PROCYON_FONT_DIR
#error "PROCYON_FONT_DIR must name the design/fonts directory"
#endif

#define PROCYON_EMBED(symbol, file)            \
    __asm__(                                   \
        ".section .rodata\n"                   \
        ".global " #symbol                     \
        "\n"                                   \
        ".type " #symbol                       \
        ", @object\n"                          \
        ".balign 16\n" #symbol                 \
        ":\n"                                  \
        ".incbin \"" PROCYON_FONT_DIR "/" file \
        "\"\n"                                 \
        ".global " #symbol "_end\n" #symbol    \
        "_end:\n"                              \
        ".byte 0\n"                            \
        ".previous\n")

PROCYON_EMBED(procyon_font_inter, "InterVariable.ttf");
PROCYON_EMBED(procyon_font_rounded, "NunitoVariable.ttf");
