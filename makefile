test_http.out : epoll.o event.o evutil.o log.o signal.o http.o buffer.o evbuffer.o  test_http.o
	gcc  -g epoll.o event.o evutil.o log.o signal.o http.o buffer.o evbuffer.o  test_http.o  -o test_http.out -lssl -lcrypto
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

http.o : http.c
	gcc -c -g http.c -o http.o

buffer.o : buffer.c
	gcc -c -g buffer.c -o buffer.o

evbuffer.o : evbuffer.c
	gcc -c -g evbuffer.c -o evbuffer.o


test_http.o : test_http.c
	gcc -c -g test_http.c -o test_http.o
clean:
	rm -rf *.o
	rm -rf test_http.out
