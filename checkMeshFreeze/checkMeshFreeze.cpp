/*
MIT License

Copyright (c) 2020 nrtkbb

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
 */
#include <stdio.h>
#include <thread>
#include <vector>
#include <deque>
#include <Windows.h>
#include <unordered_map>
#include <unordered_set>
#include <maya/MFn.h>
#include <maya/MFnPlugin.h>
#include <maya/MFnDagNode.h>
#include <maya/MFnMesh.h>
#include <maya/MItDag.h>
#include <maya/MItMeshPolygon.h>
#include <maya/MPxCommand.h>
#include <maya/MGlobal.h>
#include <maya/MSyntax.h>
#include <maya/MDistance.h>
#include <maya/MDagPath.h>
#include <maya/MDagPathArray.h>
#include <maya/MArgList.h>
#include <maya/MArgParser.h>
#include <maya/MPointArray.h>
#include <maya/MIntArray.h>
#include <maya/MSelectionList.h>
#include <maya/MThreadPool.h>

#define CheckDisplayErrorOnly(STAT,MSG)\
    if ( MStatus::kSuccess != STAT ) { \
        MGlobal::displayError(MSG);    \
    }


#define CheckDisplayError(STAT,MSG)    \
    if ( MStatus::kSuccess != STAT ) { \
        MGlobal::displayError(MSG);    \
        return MStatus::kFailure;      \
    }

#define CheckErrorReturnMThreadRetVal(STAT,MSG) \
    if ( MStatus::kSuccess != STAT ) {          \
        cerr << MSG << endl;                    \
        return (MThreadRetVal)0;                \
    }

#define CheckErrorBreak(STAT,MSG)       \
    if ( MStatus::kSuccess != STAT ) {  \
        cerr << MSG << endl;            \
        break;                          \
    }

#define CheckDisplayErrorRelease(STAT,MSG) \
    if ( MStatus::kSuccess != STAT ) {     \
        MGlobal::displayError(MSG);        \
        MThreadPool::release();            \
        return MStatus::kFailure;          \
    }

#ifdef _DEBUG
class Timer
{
public:
    Timer(MStatus* stat = nullptr) {
        if (stat != nullptr) {
            restart();
        }
        else {
            *stat = restart();
        }
    }

    MStatus restart() {
        if (!QueryPerformanceFrequency(&_freq)) {
            return MStatus::kFailure;
        }

        if (!QueryPerformanceCounter(&_start)) {
            return MStatus::kFailure;
        }

        return MStatus::kSuccess;
    }

    double  elapsed(MStatus* stat = nullptr) {
        if (!QueryPerformanceCounter(&_end)) {
            if (stat != nullptr) {
                *stat = MStatus::kFailure;
            }
            return 0.0;
        }

        if (stat != nullptr) {
            *stat = MStatus::kSuccess;
        }

        return (double)(_end.QuadPart - _start.QuadPart) / _freq.QuadPart;
    }
private:
    LARGE_INTEGER _freq;
    LARGE_INTEGER _start;
    LARGE_INTEGER _end;
};
#endif // _DEBUG

class checkMeshFreeze : public MPxCommand
{
    public:
        checkMeshFreeze();
        virtual ~checkMeshFreeze();
        MStatus doIt(const MArgList& args);
        MStatus redoIt();
        MStatus undoIt();
        bool isUndoable() const;
        static void* creator();
        static MSyntax createSyntax();
    private:
        MSelectionList _beforeSelection;
        MSelectionList _invalid;
        bool _isSelect;
};

checkMeshFreeze::checkMeshFreeze() {
}
checkMeshFreeze::~checkMeshFreeze() {
}

MSyntax checkMeshFreeze::createSyntax() {
    MSyntax syntax;

    syntax.addFlag("-s", "-select", MSyntax::kNoArg);
    return syntax;
}

typedef struct _taskDataTag
{
    // step 1
    std::deque<MDagPath> meshes;

    // step 2
    MSelectionList invalidList;

    MStatus stat;

} TaskData;

// step 1
MStatus getAllMesh(
    TaskData& taskData // in out
) {
    MItDag dagIter(MItDag::kDepthFirst, MFn::kMesh, &taskData.stat);
    CheckDisplayError(taskData.stat, "getAllMesh: could not create dagIter.\n");

    MDagPath dagPath;
    for (; !dagIter.isDone(); dagIter.next()) {
        taskData.stat = dagIter.getPath(dagPath);
        CheckDisplayError(taskData.stat, "getAllMesh: could not get dag path.\n");

        MFnDagNode dagNode(dagPath, &taskData.stat);
        CheckDisplayError(taskData.stat, "getAllMesh: could not get dag node.\n");

        if (dagNode.isIntermediateObject()) {
            continue;
        }

        taskData.meshes.push_back(dagPath);
    }
    return taskData.stat;
}

typedef struct _searchMeshFreezeTdTag {
    unsigned int    start, end;
    TaskData*       taskData;
    MSelectionList  invalidList;
    MStatus         stat;
} SearchMeshFreezeTdData;

template<typename T>
void hash_combine(size_t & seed, T const& v) {
    std::hash<T> primitive_type_hash;
    seed ^= primitive_type_hash(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

namespace std {
    template<>
    class hash<MPoint> {
    public:
        size_t operator() (const MPoint& p) const {
            std::size_t seed = 0;
            hash_combine(seed, p.x);
            hash_combine(seed, p.y);
            hash_combine(seed, p.z);
            //hash_combine(seed, p.w);
            return seed;
        }
    };
}

// step 2
 MThreadRetVal searchMeshFreezeTd(void* data) {
    SearchMeshFreezeTdData* td = (SearchMeshFreezeTdData*)data;
    TaskData* taskData = td->taskData;

    for (unsigned int i = td->start; i < td->end; ++i) {
        const MDagPath& dagPath = taskData->meshes[i];
        bool hasNonFreeze = false;

        MFnDependencyNode fnDependencyNode(dagPath.node(), &td->stat);
        CheckErrorReturnMThreadRetVal(td->stat, "searchMeshFreezeTd: could not create dependency node function.");

        MPlug pntsPlug = fnDependencyNode.findPlug("pnts", false, &td->stat);
        CheckErrorReturnMThreadRetVal(td->stat, "searchMeshFreezeTd: could not find plug.");

        const unsigned int numElm = pntsPlug.numElements(&td->stat);
        CheckErrorReturnMThreadRetVal(td->stat, "searchMeshFreezeTd: could not get num elements.");

        for (unsigned int e = 0; e < numElm; ++e) {
            MPlug pntPlug = pntsPlug.elementByPhysicalIndex(e, &td->stat);
            CheckErrorReturnMThreadRetVal(td->stat, "searchMeshFreezeTd: could not get element plug.");

            const unsigned int numChildren = pntPlug.numChildren(&td->stat);
            CheckErrorReturnMThreadRetVal(td->stat, "searchMeshFreezeTd: could not get num children.");

            for (unsigned int c = 0; c < numChildren; ++c) {
                MPlug childPlug = pntPlug.child(c, &td->stat);
                CheckErrorReturnMThreadRetVal(td->stat, "searchMeshFreezeTd: could not get child plug.");

                MDistance distance = childPlug.asMDistance(&td->stat);
                CheckErrorReturnMThreadRetVal(td->stat, "searchMeshFreezeTd: could not get distance.");

                const double value = distance.asCentimeters();
                if (value != 0.0) {
                    hasNonFreeze = true;
                    break;
                }
            }
            if (hasNonFreeze) {
                break;
            }
        }
        if (hasNonFreeze) {
            td->stat = td->invalidList.add(dagPath);
            CheckErrorReturnMThreadRetVal(td->stat, "searchMeshFreezeTd: could not add invalid list.");
        }
    }

    return (MThreadRetVal)0;
}

void searchMeshFreeze(void* data, MThreadRootTask* root) {

    const auto processor_count = std::thread::hardware_concurrency() * 10;
#ifdef _DEBUG
    cerr << "processour_count = " << processor_count << ".\n";
#endif // _DEBUG

    TaskData* taskData = (TaskData *)data;

    unsigned int size;
    if (processor_count < taskData->meshes.size()) {
        size = processor_count;
    }
    else {
        size = static_cast<unsigned int>(taskData->meshes.size());
    }

    std::vector<SearchMeshFreezeTdData> threadData(size);

    float size_f = static_cast<float>(size);
    float meshLength_f = static_cast<float>(taskData->meshes.size());

    for (unsigned int i = 0; i < size; ++i) {
        threadData[i].start = static_cast<unsigned int>(meshLength_f / size_f * i);
        threadData[i].end = static_cast<unsigned int>(meshLength_f / size_f * (i + 1));
        threadData[i].taskData = taskData;
        threadData[i].stat = MStatus::kSuccess;

        MThreadPool::createTask(searchMeshFreezeTd, (void *)&threadData[i], root);
    }

    MThreadPool::executeAndJoin(root);

    for (unsigned int i = 0; i < size; ++i) {
        if (threadData[i].invalidList.length() > 0) {
            taskData->stat = taskData->invalidList.merge(threadData[i].invalidList);
            CheckErrorBreak(taskData->stat, "searchMeshFreeze: could not merge invalid list\n");
        }

        taskData->stat = threadData[i].stat;
        CheckErrorBreak(taskData->stat, "searchMeshFreeze: thread error\n");
    }
}

MStatus checkMeshFreeze::doIt(const MArgList& args) {
    MStatus stat = MStatus::kSuccess;

#ifdef _DEBUG
    Timer timer = Timer(&stat);
    if (MStatus::kSuccess != stat) {
        return stat;
    }
#endif // _DEBUG

    MArgParser argData(syntax(), args, &stat);

    if (argData.isFlagSet("select")) {
        _isSelect = true;
        MGlobal::getActiveSelectionList(_beforeSelection);
    }
    else {
        _isSelect = false;
    }

#ifdef _DEBUG
    cerr << "parse argData = " << timer.elapsed(&stat) << "sec.\n";
    CheckDisplayError(stat, "doIt: parse argData timer elapsed error.");
    stat = timer.restart();
    CheckDisplayError(stat, "doIt: parse argData timer reset error.");
#endif // _DEBUG

    // ======================================================================
    // step 1
    TaskData taskData;
    stat = getAllMesh(taskData);
    CheckDisplayError(stat, "doIt: getAllMesh.\n");

#ifdef _DEBUG
    cerr << "getAllMesh = " << timer.elapsed(&stat) << "sec.\n";
    CheckDisplayError(stat, "doIt: getAllMesh timer elapsed error.");
    stat = timer.restart();
    CheckDisplayError(stat, "doIt: getAllMesh timer reset error.");
#endif // _DEBUG

    // ======================================================================
    // check mesh size.
    if (taskData.meshArray.size() == 0) {
        stat = redoIt();
        return stat;
    }

    // ======================================================================
    // Thread init.
    stat = MThreadPool::init();
    CheckDisplayErrorRelease(stat, "doIt: could not create threadpool.\n");

#ifdef _DEBUG
    cerr << "MThreadPool = " << timer.elapsed(&stat) << "sec.\n";
    CheckDisplayError(stat, "doIt: MThreadPool timer elapsed error.");
    stat = timer.restart();
    CheckDisplayError(stat, "doIt: MThreadPool timer reset error.");
#endif // _DEBUG

    // ======================================================================
    // step 2
    MThreadPool::newParallelRegion(searchMeshFreeze, (void *)&taskData);
    CheckDisplayErrorRelease(taskData.stat, "doIt: countMeshes error.");

#ifdef _DEBUG
    cerr << "searchMeshFreeze = " << timer.elapsed(&stat) << "sec.\n";
    CheckDisplayError(stat, "doIt: searchMeshFreeze timer elapsed error.");
    stat = timer.restart();
    CheckDisplayError(stat, "doIt: searchMeshFreeze timer reset error.");
#endif // _DEBUG

    _invalid = taskData.invalidList;

    stat = redoIt();

    return stat;
}

MStatus checkMeshFreeze::redoIt() {
    if (_isSelect) {
        MStatus stat = MGlobal::setActiveSelectionList(_invalid);
        return stat;
    }
    MStringArray results;
    MStatus stat = _invalid.getSelectionStrings(results);
    CheckDisplayError(stat, "redoIt: invalid.getSelectionStrings is failed.\n");

    setResult(results);
    return stat;
}

MStatus checkMeshFreeze::undoIt() {
    if (_isSelect) {
        MStatus stat = MGlobal::setActiveSelectionList(_beforeSelection);
        return stat;
    }
    return MStatus::kSuccess;
}

bool checkMeshFreeze::isUndoable() const {
    return true;
}

void* checkMeshFreeze::creator() {
    return new checkMeshFreeze();
}

MStatus initializePlugin(MObject obj)
{
    MFnPlugin plugin(obj, "nrtkbb", "1.0", "Any");
    plugin.registerCommand("checkMeshFreeze",
        checkMeshFreeze::creator, checkMeshFreeze::createSyntax);
    return MS::kSuccess;
}
MStatus uninitializePlugin(MObject obj)
{
    MFnPlugin plugin( obj );
    plugin.deregisterCommand("checkMeshFreeze");
    return MS::kSuccess;
}

