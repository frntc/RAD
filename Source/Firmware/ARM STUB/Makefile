PREFIX = aarch64-none-elf-

no-prefetch.bin: rad-prefetch.S
	$(PREFIX)gcc -mcpu=cortex-a53 -c rad-prefetch.S
	$(PREFIX)ld -o rad-prefetch.elf rad-prefetch.o
	$(PREFIX)objdump -D rad-prefetch.elf > rad-prefetch.lst
	$(PREFIX)objcopy rad-prefetch.elf -O binary rad-prefetch.bin

clean:
	rm -f *.o *.elf *.bin *.lst
