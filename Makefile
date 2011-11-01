CC=arm-linux-gnueabi-gcc

all:
	$(CC) flashtool.c genecc.c genecc.h debug.h -o flashtool

clean:
	rm -f flashtool
