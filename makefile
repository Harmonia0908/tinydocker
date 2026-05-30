main: 
	gcc -Wall logger/*.c util/*.c cmdparser/*.c docker/*.c main.c -lcrypto -o tinydocker

clean:
	rm -f tinydocker a.out *.o
