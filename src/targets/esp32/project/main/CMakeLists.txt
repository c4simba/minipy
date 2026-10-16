# The program (@NAME@.c: plainpy's C, the i386 machine in C variables) and its
# port (ppy_port_esp32.c). The program's C is generated: no warnings for it.
idf_component_register(SRCS "@NAME@.c" "ppy_port_esp32.c" INCLUDE_DIRS ".")
set_source_files_properties("@NAME@.c" PROPERTIES COMPILE_OPTIONS "-w;-O2;-fno-strict-aliasing")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
