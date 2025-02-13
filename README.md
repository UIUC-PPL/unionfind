## UnionFindLib

This library provides the functionality of performing union-find operations
on graphs in a distributed asynchronous fashion. The implementation is in
Charm++ and can be used in any generic Charm++-based graph applications.
The initial code was developed by Karthik Senthil.

### Example

An example application(a simple graph program) is included in the `examples`
directory. A more detailed documentation of the library usage and functionality
will be added soon.

### Currently implemented features

* Fully distributed union-find algorithm
* Simple path compression
* Connected components identification & labelling
* Threshold-based component pruning

### Todos

* TRAM integration
* Local edge optimizations
* Priority for some messages
* Testing with large graph datasets (probabilistic meshes)
* Integration with Changa

### Installation
```
$ git clone https://github.com/UIUC-PPL/unionfind.git
$ cd prefixLib
edit CHARMC path in Makefile
$ make
$ cd ..
edit CHARM_DIR and UNION_FIND_DIR in Makefile.common
$ make
```
