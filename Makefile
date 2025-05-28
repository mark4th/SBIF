all:
	gcc -O3 -o sbif  -lzstd lodepng.c sbif.c
	gcc -O3 -o dsbif -lzstd dsbif.c

clean:
	rm dsbif
	rm sbif

install:
	cp dsbif ~/bin
	cp sbif ~/bin

