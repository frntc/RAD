# Build with:  make        Deploy the .prg to your SD card / disk image yourself.
SDK  ?= ../../sdk
NAME ?= main

$(NAME).prg: $(NAME).a
	64tass -I $(SDK) $< -o $@

clean:
	rm -f $(NAME).prg $(NAME).lst
