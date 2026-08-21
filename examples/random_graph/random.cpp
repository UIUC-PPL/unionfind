#include "unionFindLib.h"
#include "random.decl.h"

CProxy_UnionFindLib libProxy;
CProxy_Main mainProxy;

class Main : public CBase_Main
{
    public:

    Main(CkArgMsg *m)
    {
        //get inputs
    }
};

class TreePiece : public CBase_TreePiece
{

    public:

    TreePiece(int randseed)
    {

    }

};

#include "random.def.h"