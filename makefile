test_main.out : epoll.o event.o evutil.o log.o test_main.o
	gcc epoll.o event.o evutil.o log.o test_main.o -o test_main.out
epoll.o : epoll.c
	gcc -c epoll.c -o epoll.o




event.o : event.c event.h
	gcc -c event.c -o event.o

evutil.o : evutil.c
	gcc -c evutil.c -o evutil.o

log.o : log.c
	gcc -c log.c -o log.o

test_main.o : test_main.c
	gcc -c test_main.c -o test_main.o


clean:
	rm -rf *.o
	rm -rf test_main.out
