#include <string>
#include <vector>
#include <sstream>

#define USE_PROTEIN
//#define USE_SAMPLE

// vertex structure for given graph
#ifdef USE_PROTEIN
struct proteinVertex {
    long int id;
    char complexType;
    float x,y,z;
};
#endif

#ifdef USE_SAMPLE
struct proteinVertex {
    long int id;
    std::string type;
};
#endif

// utility function declarations
void seekToLine(FILE *fp, long int lineNum);
std::pair<long int, long int> ReadEdge(FILE *fp); 
proteinVertex ReadVertex(FILE *fp);


void populateMyVertices(std::vector<proteinVertex>& myVertices, int nVertices, int nChares, int chareIdx, FILE *fp) {
    int startVid = chareIdx + 1;
#ifdef USE_PROTEIN
    long int lineNum = startVid + 3;
#endif
#ifdef USE_SAMPLE    
    long int lineNum = startVid + 20;
    //printf("[%d] lineNum: %ld\n", chareIdx, lineNum); 
#endif
    seekToLine(fp, lineNum);

    int target_per_chare = nVertices / nChares;

    while (startVid <= nVertices) {
        proteinVertex v = ReadVertex(fp);
        if((startVid-1) / target_per_chare == chareIdx)
        {
            myVertices.push_back(v);
            //if(chareIdx==0) printf("Chare 0 has vertex %d\n", v.id);
        }
        else if((chareIdx==nChares-1)&&(((startVid-1) / target_per_chare)>=nChares))
        {
            myVertices.push_back(v);
            //printf("Chare %d has vertex %d\n", chareIdx, v.id);
        }
        /*
        if ((startVid-1) % nChares == chareIdx) {
            myVertices.push_back(v);
        }
            */

        startVid++;
    }
}

void populateMyEdges(std::vector< std::pair<long int, long int> > *myEdges, int nMyEdges, int eRatio, int chareIdx, FILE *fp, int totalNVertices) {
   int startEid = (eRatio * chareIdx) + 1;
#ifdef USE_PROTEIN   
   long int lineNum = startEid + totalNVertices + 3; //3 starting lines
#endif
#ifdef USE_SAMPLE
   long int lineNum = startEid + totalNVertices + 20 + 1;
#endif
   //printf("[%d]lineNum : %ld\n", chareIdx, lineNum);
   seekToLine(fp, lineNum);
   for (int i = 0; i < nMyEdges; i++) {
       std::pair<long int, long int> edge = ReadEdge(fp);
       myEdges->push_back(edge);
   }
}

void seekToLine(FILE *fp, long int lineNum) {
    long int i = 1, byteCount = 0;
    if (fp != NULL) {
        char line[256];
        while (fgets(line, sizeof(line), fp) != NULL) {
            if (i == lineNum) {
                break;
            }
            else {
                i++;
                byteCount += strlen(line);
            }
        }
        // seek to byteCount
        fseek(fp, byteCount, SEEK_SET);
    }
    else {
        printf("File not found\n");
    }
}

void split(char *str, char delim, std::vector<std::string> *elems) {
    std::stringstream ss;
    ss << (char*) str;
    std::string item;
    while(getline(ss, item, delim)) {
        elems->push_back(item);
    }
}

void sanitizeFields(std::vector<std::string> *fields) {
    std::vector<std::string>::iterator iter;
    for (iter = fields->begin(); iter != fields->end();) {
        if (iter->find_first_not_of(' ') == std::string::npos) {
            iter = fields->erase(iter);
        }
        else {
            iter++;
        }
    }
    iter = fields->end();
    //iter->pop_back();
}

proteinVertex ReadVertex(FILE *fp) {
    char line[256];
    fgets(line, sizeof(line), fp);
    line[strcspn(line, "\n")] = 0; // removes trailing newline character
    std::vector<std::string> vertexFields;
    split(line, ' ', &vertexFields);
    sanitizeFields(&vertexFields);

    // error check for protein files only
    /*if (vertexFields.size() != 7 || vertexFields[0] != "v") {
        printf("Error in vertex format\n");
        exit(0);
    }*/
    if (vertexFields[0] != "v") {
        printf("Error in vertex format\n Obtain vertex: %s\n", line);
        exit(0);
    }

    proteinVertex newVertex;
    newVertex.id = stol(vertexFields[1]);
#ifdef USE_SAMPLE
    newVertex.type = vertexFields[2];
#endif
#ifdef USE_PROTEIN   
    newVertex.complexType = vertexFields[2][0];
    newVertex.x = stof(vertexFields[4]);
    newVertex.y = stof(vertexFields[5]);
    newVertex.z = stof(vertexFields[6]);
#endif
    return newVertex;
}

std::pair<long int, long int> ReadEdge(FILE *fp) {
    char line[256];
    fgets(line, sizeof(line), fp);
    line[strcspn(line, "\n")] = 0;

    std::vector<std::string> edgeFields;
    split(line, ' ', &edgeFields);

    // error check for protein files
    /*if (edgeFields.size() != 4 || edgeFields[0] != "u") {
        printf("Error in edge format\nObtained edge: %s\n", line);
        exit(0);
    }*/
    if (edgeFields.size() != 4 || (edgeFields[0] != "e" && edgeFields[0] != "u")) {
        printf("Error in edge format\nObtained edge: %s\n", line);
        exit(0);
    }

    std::pair<long int,long int> newEdge;
    newEdge.first = stol(edgeFields[1]);
    newEdge.second = stol(edgeFields[2]);

    return newEdge;
}


