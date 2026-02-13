include ../../Makefile.common

all: random

random: random.o
	$(CHARMC) ${LD_OPTS} -o $@ random.o ${UNION_FIND_LIBS}

random.o: random.cpp random.decl.h random.def.h
	$(CHARMC) -c ${OPTS} ${UNION_FIND_INC} random.cpp

random.decl.h random.def.h: random.ci
	$(CHARMC) random.ci

clean:
	rm -f *.decl.h *.def.h conv-host *.o random charmrun
	rm -f obtained.*