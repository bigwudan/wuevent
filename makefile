test_main.out : epoll.o event.o evutil.o log.o signal.o test_main.o
	gcc -g epoll.o event.o evutil.o log.o signal.o test_main.o -o test_main.out
epoll.o : epoll.c
	gcc -c -g epoll.c -o epoll.o

event.o : event.c event.h
	gcc -c -g event.c -o event.o

evutil.o : evutil.c
	gcc -c -g evutil.c -o evutil.o

log.o : log.c
	gcc -c -g log.c -o log.o

signal.o : signal.c evsignal.h
	gcc -c -g signal.c -o signal.o

test_main.o : test_main.c
	gcc -c -g test_main.c -o test_main.o
clean:
	rm -rf *.o
	rm -rf test_main.out
