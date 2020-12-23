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
#include <maya/MString.h>
#include <maya/MFn.h>
#include <maya/MFnPlugin.h>
#include <maya/MFnDagNode.h>
#include <maya/MFnMesh.h>
#include <maya/MItDag.h>
#include <maya/MItMeshPolygon.h>
#include <maya/MPxCommand.h>
#include <maya/MGlobal.h>
#include <maya/MSyntax.h>
#include <maya/MDagPath.h>
#include <maya/MDagPathArray.h>
#include <maya/MArgList.h>
#include <maya/MArgParser.h>
#include <maya/MSelectionList.h>
#include <maya/MThreadPool.h>

namespace
{
    // select argument
    const char *selectArgName = "-s";
    const char *selectLongArgName = "-select";

    // uv set argument
    const char *uvSetArgName = "-uvs";
    const char *uvSetLongArgName = "-uvSet";

    // all uv set argument
    const char *allUVSetArgName = "-all";
    const char *allUVSetLongArgName = "-allUVSet";
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

class checkMeshUVFlip : public MPxCommand
{
    public:
        checkMeshUVFlip();
        virtual ~checkMeshUVFlip();
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

checkMeshUVFlip::checkMeshUVFlip()
    : _beforeSelection()
    , _invalid()
    , _fIsSelect(false)
{
}
checkMeshUVFlip::~checkMeshUVFlip() {
}

MSyntax checkMeshUVFlip::createSyntax() {
    MSyntax syntax;

    syntax.addFlag(selectArgName, selectLongArgName, MSyntax::kNoArg);
    syntax.addFlag(uvSetArgName, uvSetLongArgName, MSyntax::kString);
    syntax.addFlag(allUVSetArgName, allUVSetLongArgName, MSyntax::kNoArg);
    return syntax;
}

typedef struct _taskDataTag
{
    // flags
    MString uvSet;
    bool    allUVSet;

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

typedef struct _searchMeshUVFlipTdTag {
    unsigned int    start, end;
    TaskData*       taskData;
    MSelectionList  invalidList;
    MStatus         stat;
} SearchMeshUVFlipTdData;

// step 2
 MThreadRetVal searchMeshUVFlipTd(void* data) {
    SearchMeshUVFlipTdData* td = (SearchMeshUVFlipTdData*)data;
    TaskData* taskData = td->taskData;

    MStringArray uvSetNames;
    MStatus numPolygonsStat;
    MStatus isUVReversedStat;
    for (unsigned int i = td->start; i < td->end; ++i) {
        const MDagPath& dagPath = taskData->meshArray[i];

        MFnMesh fnMesh(dagPath, &td->stat);
        CheckErrorReturnMThreadRetVal(td->stat, "searchMeshUVFlipTd: could not create MFnMesh.");

        const int numPolygons = fnMesh.numPolygons(&numPolygonsStat);
        CheckErrorReturnMThreadRetVal(numPolygonsStat, "searchMeshUVFlipTd: could not get num polygons.");

        if (numPolygons == 0) {
            // If this mesh don't have any face, skip it.
            MGlobal::displayWarning(dagPath.partialPathName() + " is zero polygon. skip.");
            continue;
        }

        uvSetNames.clear();
        td->stat = fnMesh.getUVSetNames(uvSetNames);
        CheckErrorReturnMThreadRetVal(td->stat, "searchMeshUVFlipTd: could not get uv set names.");

        // check uv set names.
        if (uvSetNames.indexOf(taskData->uvSet) == -1 && !taskData->allUVSet) {
            // Skip if this mesh don't have the specify uvSet wanted to.
            MGlobal::displayWarning(dagPath.partialPathName() + " has't the " + taskData->uvSet + " uvSet. skip.");
            continue;
        }

        MItMeshPolygon itMeshPolygon(dagPath, MObject::kNullObj, &td->stat);
        CheckErrorReturnMThreadRetVal(td->stat, "searchMeshUVFlipTd: could not create MItMeshPolygon.");

        for (; !itMeshPolygon.isDone(); itMeshPolygon.next()) {
            if (taskData->allUVSet) {
                for (unsigned int ii = 0; ii < uvSetNames.length(); ++ii) {
                    const MString& uvSetName = uvSetNames[ii];

                    bool isFlip = itMeshPolygon.isUVReversed(&uvSetName, &isUVReversedStat);
                    if (isFlip || isUVReversedStat != MStatus::kSuccess) {
                        MObject faceComponent = itMeshPolygon.currentItem(&td->stat);
                        CheckErrorReturnMThreadRetVal(td->stat,
                            "searchMeshUVFlipTd: could not get face component.");

                        td->stat = td->invalidList.add(dagPath, faceComponent);
                        CheckErrorReturnMThreadRetVal(td->stat,
                            "searchMeshUVFlipTd: could not add invalid list.");
                    }
                }
                continue;
            }

            bool isFlip = itMeshPolygon.isUVReversed(&taskData->uvSet, &isUVReversedStat);

            if (isFlip || isUVReversedStat != MStatus::kSuccess) {
                MObject faceComponent = itMeshPolygon.currentItem(&td->stat);
                CheckErrorReturnMThreadRetVal(td->stat,
                    "searchMeshUVFlipTd: could not get face component.");

                td->stat = td->invalidList.add(dagPath, faceComponent);
                CheckErrorReturnMThreadRetVal(td->stat,
                    "searchMeshUVFlipTd: could not add invalid list.");
            }
        }
    }

    return (MThreadRetVal)0;
}

void searchMeshUVFlip(void* data, MThreadRootTask* root) {

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

    std::vector<SearchMeshUVFlipTdData> threadData(size);

    float size_f = static_cast<float>(size);
    float meshLength_f = static_cast<float>(taskData->meshArray.size());

    for (unsigned int i = 0; i < size; ++i) {
        threadData[i].start = static_cast<unsigned int>(meshLength_f / size_f * static_cast<float>(i));
        threadData[i].end = static_cast<unsigned int>(meshLength_f / size_f * static_cast<float>(i + 1));
        threadData[i].taskData = taskData;
        threadData[i].stat = MStatus::kSuccess;

        MThreadPool::createTask(searchMeshUVFlipTd, (void *)&threadData[i], root);
    }

    MThreadPool::executeAndJoin(root);

    for (unsigned int i = 0; i < size; ++i) {
        if (threadData[i].invalidList.length() > 0) {
            taskData->stat = taskData->invalidList.merge(threadData[i].invalidList);
            CheckErrorBreak(taskData->stat, "searchMeshUVFlip: could not merge invalid list");
        }

        taskData->stat = threadData[i].stat;
        CheckErrorBreak(taskData->stat, "searchMeshUVFlip: thread error");
    }
}

MStatus checkMeshUVFlip::doIt(const MArgList& args) {
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

    if (argData.isFlagSet(allUVSetArgName)) {
        taskData.allUVSet = true;
    }

    if (argData.isFlagSet(uvSetArgName)) {
        stat = argData.getFlagArgument(uvSetArgName, 0, taskData.uvSet);
        CheckDisplayError(stat, "doIt: could not get uvSet argument data.");
    }
    else {
        taskData.uvSet = MString("map1");
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
    // check mesh size.
    if (taskData.meshArray.size() == 0) {
        stat = redoIt();
        return stat;
    }

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
    MThreadPool::newParallelRegion(searchMeshUVFlip, (void *)&taskData);
    CheckDisplayErrorRelease(taskData.stat, "doIt: searchMeshUVFlip error.");

#ifdef _DEBUG
    cerr << "searchMeshUVFlip = " << timer.elapsed(&stat) << "sec.\n";
    CheckDisplayError(stat, "doIt: searchMeshUVFlip timer elapsed error.");
    stat = timer.restart();
    CheckDisplayError(stat, "doIt: searchMeshUVFlip timer reset error.");
#endif // _DEBUG

    _invalid = taskData.invalidList;

    stat = redoIt();

    return stat;
}

MStatus checkMeshUVFlip::redoIt() {
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

MStatus checkMeshUVFlip::undoIt() {
    if (_fIsSelect) {
        MStatus stat = MGlobal::setActiveSelectionList(_beforeSelection);
        return stat;
    }
    return MStatus::kSuccess;
}

bool checkMeshUVFlip::isUndoable() const {
    return true;
}

void* checkMeshUVFlip::creator() {
    return new checkMeshUVFlip();
}

MStatus initializePlugin(MObject obj)
{
    MFnPlugin plugin(obj, "nrtkbb", "1.0", "Any");
    plugin.registerCommand("checkMeshUVFlip",
        checkMeshUVFlip::creator, checkMeshUVFlip::createSyntax);
    return MS::kSuccess;
}
MStatus uninitializePlugin(MObject obj)
{
    MFnPlugin plugin( obj );
    plugin.deregisterCommand("checkMeshUVFlip");
    return MS::kSuccess;
}

