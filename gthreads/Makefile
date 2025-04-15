CFLAGS = -g -Wall
LDLIBS = -lm

all: gthr_demo semaphore_test

gthr_demo: gthr.o gthr_switch.o main.o gthr_params.o
	$(CC) -o $@ $^ $(LDLIBS)

semaphore_test: gthr.o gthr_switch.o semaphore_test.o gthr_params.o
	$(CC) -o $@ $^ $(LDLIBS)

.S.o:
	as -o $@ $^

.PHONY: clean
clean:
	rm -f *.o gthr_demo semaphore_test
