CHARMC=/Users/raghu/work/charm/charm/bin/charmc $(OPTS)

all: libprefix.a

libprefix.a: prefixBalance.o
	$(CHARMC) prefixBalance.o -o libprefix.a -language charm++ # -DCK_TEMPLATES_ONLY

prefixBalance.o : prefixBalance.C prefixBalance.def.h prefixBalance.decl.h
	$(CHARMC) -c prefixBalance.C

prefixBalance.def.h prefixBalance.decl.h : prefixBalance.ci
	$(CHARMC) prefixBalance.ci

clean:
	rm -f conv-host *.o charmrun test
	rm -f *.def.h *.decl.h
	rm -f *.log.gz *.projrc *.topo *.sts *.sum
	rm -f libprefix.a

