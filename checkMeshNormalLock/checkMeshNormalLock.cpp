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
#include <string>
#include <iostream>
#include <fstream>
#include <thread>
#include <vector>
#include <deque>
#include <Windows.h>
#include <maya/MString.h>
#include <maya/MFn.h>
#include <maya/MFnPlugin.h>
#include <maya/MFnDagNode.h>
#include <maya/MFnMesh.h>
#include <maya/MItDag.h>
#include <maya/MItMeshFaceVertex.h>
#include <maya/MItMeshVertex.h>
#include <maya/MItMeshPolygon.h>
#include <maya/MPxCommand.h>
#include <maya/MGlobal.h>
#include <maya/MSyntax.h>
#include <maya/MDagPath.h>
#include <maya/MDagPathArray.h>
#include <maya/MArgList.h>
#include <maya/MArgParser.h>
#include <maya/MSelectionList.h>
#include <maya/MMutexLock.h>
#include <maya/MThreadPool.h>

namespace
{
    // select argument
    const char *selectArgName = "-s";
    const char *selectLongArgName = "-select";
};

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

#define _DEBUG

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

class File
{
public:
    File(const char* fileName) : _file(fileName) {};
    virtual ~File() {
        _file.close();
    }
    void write(const char* line) {
        _file << line << std::endl;
    }
    void write(const std::string& line) {
        _file << line.c_str() << std::endl;
    }
private:
    std::ofstream _file;
};
#endif // _DEBUG

class checkMeshNormalLock : public MPxCommand
{
    public:
        checkMeshNormalLock();
        virtual ~checkMeshNormalLock();
        MStatus doIt(const MArgList& args);
        MStatus redoIt();
        MStatus undoIt();
        bool isUndoable() const;
        static void* creator();
        static MSyntax createSyntax();
    private:
        MSelectionList _beforeSelection;
        MSelectionList _invalid;

        bool _fIsSelect;
};

checkMeshNormalLock::checkMeshNormalLock()
    : _beforeSelection()
    , _invalid()
    , _fIsSelect(false)
{
}
checkMeshNormalLock::~checkMeshNormalLock() {
}

MSyntax checkMeshNormalLock::createSyntax() {
    MSyntax syntax;

    syntax.addFlag(selectArgName, selectLongArgName, MSyntax::kNoArg);
    return syntax;
}

typedef struct _taskDataTag
{
    // step 1
    std::deque<MDagPath> meshArray;

    // step 2
    MSelectionList invalidList;

    MStatus stat;

} TaskData;

// step 1
MStatus getAllMesh(
    TaskData& taskData // in out
) {
    MItDag dagIter(MItDag::kDepthFirst, MFn::kMesh, &taskData.stat);
    CheckDisplayError(taskData.stat, "getAllMesh: could not create dagIter.");

    MDagPath dagPath;
    for (; !dagIter.isDone(); dagIter.next()) {
        taskData.stat = dagIter.getPath(dagPath);
        CheckDisplayError(taskData.stat, "getAllMesh: could not get dag path.");

        MFnDagNode dagNode(dagPath, &taskData.stat);
        CheckDisplayError(taskData.stat, "getAllMesh: could not get dag node.");

        if (dagNode.isIntermediateObject()) {
            continue;
        }

        taskData.meshArray.push_back(dagPath);
    }
    return taskData.stat;
}

typedef struct _searchMeshNormalLockTdTag {
    unsigned int    start, end;
    TaskData*       taskData;
    MSelectionList  invalidList;
    MMutexLock*     mutex;
    MStatus         stat;
} SearchMeshNormalLockTdData;

// step 2
 MThreadRetVal searchMeshNormalLockTd(void* data) {
    SearchMeshNormalLockTdData* td = (SearchMeshNormalLockTdData*)data;
    TaskData* taskData = td->taskData;

    bool isNormalLock;
    for (unsigned int i = td->start; i < td->end; ++i) {
        const MDagPath& dagPath = taskData->meshArray[i];

        MFnMesh fnMesh(dagPath, &td->stat);
        CheckErrorReturnMThreadRetVal(td->stat, "searchMeshNormalLockTd: could not create MFnMesh.");

        const int numNormals = fnMesh.numNormals(&td->stat);
        CheckErrorReturnMThreadRetVal(td->stat, "searchMeshNormalLockTd: could not get num normals.");

        for (int n = 0; n < numNormals; ++n) {
            isNormalLock = fnMesh.isNormalLocked(n, &td->stat);
            CheckErrorReturnMThreadRetVal(td->stat, "searchMeshNormalLockTd: could not get num normals.");

            if (isNormalLock) {
                td->stat = td->invalidList.add(dagPath);
                CheckErrorReturnMThreadRetVal(td->stat, "searchMeshNormalLockTd: could not add invalid list.");
                break;
            }
        }
    }

    return (MThreadRetVal)0;
}

void searchMeshNormalLock(void* data, MThreadRootTask* root) {

    const auto processor_count = std::thread::hardware_concurrency() * 10;
#ifdef _DEBUG
    cerr << "processour_count = " << processor_count << ".\n";
#endif // _DEBUG

    TaskData* taskData = (TaskData *)data;

    unsigned int size;
    if (processor_count < taskData->meshArray.size()) {
        size = processor_count;
    }
    else {
        size = static_cast<unsigned int>(taskData->meshArray.size());
    }

    std::vector<SearchMeshNormalLockTdData> threadData(size);
    MMutexLock mutex;

    float size_f = static_cast<float>(size);
    float meshLength_f = static_cast<float>(taskData->meshArray.size());

    for (unsigned int i = 0; i < size; ++i) {
        threadData[i].start = static_cast<unsigned int>(meshLength_f / size_f * static_cast<float>(i));
        threadData[i].end = static_cast<unsigned int>(meshLength_f / size_f * static_cast<float>(i + 1));
        threadData[i].taskData = taskData;
        threadData[i].mutex = &mutex;
        threadData[i].stat = MStatus::kSuccess;

        MThreadPool::createTask(searchMeshNormalLockTd, (void *)&threadData[i], root);
    }

    MThreadPool::executeAndJoin(root);

    for (unsigned int i = 0; i < size; ++i) {
        if (threadData[i].invalidList.length() > 0) {
            taskData->stat = taskData->invalidList.merge(threadData[i].invalidList);
            CheckErrorBreak(taskData->stat, "searchMeshNormalLock: could not merge invalid list");
        }

        taskData->stat = threadData[i].stat;
        CheckErrorBreak(taskData->stat, "searchMeshNormalLock: thread error");
    }
}

MStatus checkMeshNormalLock::doIt(const MArgList& args) {
    MStatus stat = MStatus::kSuccess;

#ifdef _DEBUG
    Timer timer = Timer(&stat);
    if (MStatus::kSuccess != stat) {
        return stat;
    }
#endif // _DEBUG

    MArgParser argData(syntax(), args, &stat);
    CheckDisplayError(stat, "doIt: argument syntax error.");
    TaskData taskData;

    if (argData.isFlagSet(selectArgName)) {
        _fIsSelect = true;
        MGlobal::getActiveSelectionList(_beforeSelection);
    }

#ifdef _DEBUG
    cerr << "parse argData = " << timer.elapsed(&stat) << "sec.\n";
    CheckDisplayError(stat, "doIt: parse argData timer elapsed error.");
    stat = timer.restart();
    CheckDisplayError(stat, "doIt: parse argData timer reset error.");
#endif // _DEBUG

    // ======================================================================
    // step 1
    stat = getAllMesh(taskData);
    CheckDisplayError(stat, "doIt: getAllMesh.");

#ifdef _DEBUG
    cerr << "getAllMesh = " << timer.elapsed(&stat) << "sec.\n";
    CheckDisplayError(stat, "doIt: getAllMesh timer elapsed error.");
    stat = timer.restart();
    CheckDisplayError(stat, "doIt: getAllMesh timer reset error.");
#endif // _DEBUG

    // ======================================================================
    // Thread init.
    stat = MThreadPool::init();
    CheckDisplayError(stat, "doIt: could not create threadpool.");

#ifdef _DEBUG
    cerr << "MThreadPool = " << timer.elapsed(&stat) << "sec.\n";
    CheckDisplayError(stat, "doIt: MThreadPool timer elapsed error.");
    stat = timer.restart();
    CheckDisplayError(stat, "doIt: MThreadPool timer reset error.");
#endif // _DEBUG

    // ======================================================================
    // step 2
    MThreadPool::newParallelRegion(searchMeshNormalLock, (void *)&taskData);
    CheckDisplayErrorRelease(taskData.stat, "doIt: countMeshes error.");

#ifdef _DEBUG
    cerr << "searchMeshNormalLock = " << timer.elapsed(&stat) << "sec.\n";
    CheckDisplayError(stat, "doIt: searchMeshNormalLock timer elapsed error.");
    stat = timer.restart();
    CheckDisplayError(stat, "doIt: searchMeshNormalLock timer reset error.");
#endif // _DEBUG

    _invalid = taskData.invalidList;

    stat = redoIt();

    return stat;
}

MStatus checkMeshNormalLock::redoIt() {
    if (_fIsSelect) {
        MStatus stat = MGlobal::setActiveSelectionList(_invalid);
        return stat;
    }
    MStringArray results;
    MStatus stat = _invalid.getSelectionStrings(results);
    CheckDisplayError(stat, "redoIt: invalid.getSelectionStrings is failed.");

    setResult(results);
    return stat;
}

MStatus checkMeshNormalLock::undoIt() {
    if (_fIsSelect) {
        MStatus stat = MGlobal::setActiveSelectionList(_beforeSelection);
        return stat;
    }
    return MStatus::kSuccess;
}

bool checkMeshNormalLock::isUndoable() const {
    return true;
}

void* checkMeshNormalLock::creator() {
    return new checkMeshNormalLock();
}

MStatus initializePlugin(MObject obj)
{
    MFnPlugin plugin(obj, "nrtkbb", "1.0", "Any");
    plugin.registerCommand("checkMeshNormalLock",
        checkMeshNormalLock::creator, checkMeshNormalLock::createSyntax);
    return MS::kSuccess;
}
MStatus uninitializePlugin(MObject obj)
{
    MFnPlugin plugin( obj );
    plugin.deregisterCommand("checkMeshNormalLock");
    return MS::kSuccess;
}

