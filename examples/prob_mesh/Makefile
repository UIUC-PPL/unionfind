include ../../Makefile.common

all: mesh

mesh: mesh.o
	$(CHARMC) ${LD_OPTS} -o $@ mesh.o ${UNION_FIND_LIBS} -tracemode projections

mesh.o : mesh.C mesh.decl.h mesh.def.h
	$(CHARMC) -c ${OPTS} ${UNION_FIND_INC} mesh.C

mesh.decl.h mesh.def.h : mesh.ci
	$(CHARMC) -E mesh.ci

clean:
	rm -f *.decl.h *.def.h conv-host *.o mesh charmrun

cleanp:
	rm -f *.sts *.gz *.projrc *.topo *.out

cleanpl:
	rm -f *.sts *.gz *.projrc *.topo *.out *.log

test: all
	./charmrun +p4 ./mesh 26 2 0.4 ++local

demo: all
	./charmrun +p4 ./mesh 256 64 0.6 ++local
