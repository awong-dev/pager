# detools 0.53.0 sequential patch applier (BSD-2-Clause, see LICENSE). Only the
# uncompressed patch kind is built: the OTA object is zlib-wrapped and inflated
# by the ROM tinfl before it reaches detools (docs/OTA_DESIGN.md D4).
idf_component_register(SRCS "detools.c" INCLUDE_DIRS ".")
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    DETOOLS_CONFIG_FILE_IO=0
    DETOOLS_CONFIG_COMPRESSION_LZMA=0
    DETOOLS_CONFIG_COMPRESSION_CRLE=0
    DETOOLS_CONFIG_COMPRESSION_HEATSHRINK=0)
