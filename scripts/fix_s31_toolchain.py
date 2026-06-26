Import("env")

board = env.BoardConfig()
mcu = board.get("build.mcu", "")

if mcu == "esp32s31":
    env.Replace(
        AR="riscv32-esp-elf-gcc-ar",
        AS="riscv32-esp-elf-as",
        CC="riscv32-esp-elf-gcc",
        CXX="riscv32-esp-elf-g++",
        GDB="riscv32-esp-elf-gdb",
        OBJCOPY="riscv32-esp-elf-objcopy",
        RANLIB="riscv32-esp-elf-gcc-ranlib",
        SIZETOOL="riscv32-esp-elf-size",
    )
