all: fat fat_fuse

fat:
	gcc fat.c -o fat

fat.o: fat.c
	gcc -Wall -c fat.c -DFUSE_MODE -o fat.o

fat_fuse.o: fat_fuse.c
	gcc -Wall -c fat_fuse.c `pkg-config fuse3 --cflags` -o fat_fuse.o

fat_fuse: fat.o fat_fuse.o
	gcc fat.o fat_fuse.o `pkg-config fuse3 --libs` -o fat_fuse

clean:
	rm -f fat fat_fuse *.o
