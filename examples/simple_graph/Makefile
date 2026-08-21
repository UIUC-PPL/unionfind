include ../../Makefile.common

all: graph

graph: graph.o
	$(CHARMC) ${LD_OPTS} -o $@ graph.o ${UNION_FIND_LIBS} -tracemode projections

graph.o : graph.C graph-io.h graph.decl.h graph.def.h
	$(CHARMC) -c ${OPTS} ${UNION_FIND_INC} graph.C

graph.decl.h graph.def.h : graph.ci
	$(CHARMC) -E graph.ci

clean:
	rm -f *.decl.h *.def.h conv-host *.o graph charmrun
	rm -f obtained.*

cleanp:
	rm -f *.sts *.gz *.projrc *.topo *.out

cleanpl:
	rm -f *.sts *.gz *.projrc *.topo *.out *.log

test-all: all
	./run_tests
