event.o : event.c event.h
	gcc -c event.c -o event.o && rm -rf *.o


clean:
	rm -rf *.o
