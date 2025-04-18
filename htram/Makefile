include ../Makefile.common
CFLAGS = -DHISTO

all: libhtram_group.a

# note: make sure charmc is an smp build of charm
libhtram_group.a: htram_group.o
	$(CHARMC) $< -o $@ -language charm++

htram_group.o: htram_group.C htram_group.def.h htram_group.decl.h
	$(CHARMC) -O3 -c htram_group.C $(CFLAGS)

htram_group.def.h htram_group.decl.h: htram_group.ci
	$(CHARMC) htram_group.ci

clean:
	rm -f *.o *.decl.h *.def.h charmrun* *.stamp