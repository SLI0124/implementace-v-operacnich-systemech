CFLAGS = -g -Wall
LDLIBS = -lm

gthr_demo: gthr.o gthr_switch.o main.o
	$(CC) -o $@ $^ $(LDLIBS)

.S.o:
	as -o $@ $^

.PHONY: clean
clean:
	rm -f *.o gthr_demo
