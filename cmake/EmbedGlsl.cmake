# cmake -DINPUT=file.glsl -DOUTPUT=header.h -DNAME=MACRO -P EmbedGlsl.cmake: #define MACRO as the file's text (a raw
# string literal, for concatenation into the OpenGL shader sources). The file must hold no preprocessor lines.
file(READ "${INPUT}" text)
file(WRITE "${OUTPUT}" "// Generated from ${INPUT}; do not edit.\n#define ${NAME} R\"IKVGLSL(\n${text})IKVGLSL\"\n")
