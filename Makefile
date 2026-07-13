# Define our compiler tools and target architecture flags
AS = nasm
CC = g++
LD = ld

# Freestanding C++ target compilation flags matrix with local include pathways
ASFLAGS = -f elf32
CFLAGS  = -m32 -c -ffreestanding -O2 -Wall -Wextra -fno-exceptions -fno-rtti -Iinclude -std=c++20
LDFLAGS = -m elf_i386 -T linker.ld

all: bin/fontaineos.bin bin/disk.img

bin/fontaineos.bin: src/boot.o src/kernel.o src/gdt.o src/idt.o src/timer.o src/keyboard.o src/pmm.o src/vmm.o src/heap.o src/task.o src/ata.o src/fontfs.o
	$(LD) $(LDFLAGS) -o bin/fontaineos.bin src/boot.o src/kernel.o src/gdt.o src/idt.o src/timer.o src/keyboard.o src/pmm.o src/vmm.o src/heap.o src/task.o src/ata.o src/fontfs.o

# Automatically generate a clean 10MB virtual hard drive image if one is missing
bin/disk.img:
	dd if=/dev/zero of=bin/disk.img bs=1M count=10

src/boot.o: src/boot.s
	$(AS) $(ASFLAGS) src/boot.s -o src/boot.o

src/kernel.o: src/kernel.cpp
	$(CC) $(CFLAGS) src/kernel.cpp -o src/kernel.o

src/gdt.o: src/gdt.cpp
	$(CC) $(CFLAGS) src/gdt.cpp -o src/gdt.o

src/idt.o: src/idt.cpp
	$(CC) $(CFLAGS) src/idt.cpp -o src/idt.o

src/timer.o: src/timer.cpp
	$(CC) $(CFLAGS) src/timer.cpp -o src/timer.o

src/keyboard.o: src/keyboard.cpp
	$(CC) $(CFLAGS) src/keyboard.cpp -o src/keyboard.o

src/pmm.o: src/pmm.cpp
	$(CC) $(CFLAGS) src/pmm.cpp -o src/pmm.o

src/vmm.o: src/vmm.cpp
	$(CC) $(CFLAGS) src/vmm.cpp -o src/vmm.o

src/heap.o: src/heap.cpp
	$(CC) $(CFLAGS) src/heap.cpp -o src/heap.o

src/task.o: src/task.cpp
	$(CC) $(CFLAGS) src/task.cpp -o src/task.o

src/ata.o: src/ata.cpp
	$(CC) $(CFLAGS) src/ata.cpp -o src/ata.o

src/fontfs.o: src/fontfs.cpp
	$(CC) $(CFLAGS) src/fontfs.cpp -o src/fontfs.o

# Fixed: Pass our newly generated disk image to QEMU's primary master IDE bus line!
run: bin/fontaineos.bin bin/disk.img
	qemu-system-i386 -kernel bin/fontaineos.bin -drive file=bin/disk.img,format=raw,index=0,media=disk


clean:
	rm -f src/*.o bin/*.bin bin/disk.img
