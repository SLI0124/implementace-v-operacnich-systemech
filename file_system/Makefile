all: fat fat_fuse

fat:
	gcc fat.c -o fat

fat_fuse:
	gcc -Wall -g fat_fuse.c `pkg-config fuse3 --cflags --libs` -o fat_fuse

clean:
	rm -f fat fat_fuse
