include Makefile.common

all: lib

lib: libunionFind.a

libunionFind.a : unionFindLib.o
	$(CHARMC) ${LD_OPTS} -o libunionFind.a unionFindLib.o ${PREFIX_LIBS} ${HTRAM_LIBS}

unionFindLib.o : unionFindLib.C types.h unionFindLib.h unionFindLib.decl.h unionFindLib.def.h
	$(CHARMC) -c ${OPTS} ${PREFIX_INC} ${HTRAM_INC} $<

unionFindLib.decl.h unionFindLib.def.h : unionFindLib.ci
	$(CHARMC) -E $<

clean:
	rm -f *.decl.h *.def.h conv-host *.o charmrun
	rm -f libunionFind.a

cleanp:
	rm -f *.sts *.gz *.projrc *.topo *.out

cleanpl:
	rm -f *.sts *.gz *.projrc *.topo *.out *.log
