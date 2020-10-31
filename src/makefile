compile: test4.c sut.o
	gcc -o test4 test4.c sut.o -lpthread

sut.o: sut.c
	gcc -o sut.o -c sut.c -lpthread

clean:
	rm sut.o