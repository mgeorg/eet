
EXTRAFILES=

EXECUTABLES=eet

all:	$(EXECUTABLES)

eet:	eet.c Makefile
	gcc -g -o eet eet.c

clean:	
	rm -f $(EXECUTABLES) $(EXTRAFILES)

