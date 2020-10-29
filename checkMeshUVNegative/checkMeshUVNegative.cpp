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

class checkMeshUVNegative : public MPxCommand
{
    public:
        checkMeshUVNegative();
        virtual ~checkMeshUVNegative();
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

checkMeshUVNegative::checkMeshUVNegative()
    : _beforeSelection()
    , _invalid()
    , _fIsSelect(false)
{
}
checkMeshUVNegative::~checkMeshUVNegative() {
}

MSyntax checkMeshUVNegative::createSyntax() {
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

typedef struct _searchMeshUVNegativeTdTag {
    unsigned int    start, end;
    TaskData*       taskData;
    MSelectionList  invalidList;
    MStatus         stat;
} SearchMeshUVNegativeTdData;

// step 2
 MThreadRetVal searchMeshUVNegativeTd(void* data) {
    SearchMeshUVNegativeTdData* td = (SearchMeshUVNegativeTdData*)data;
    TaskData* taskData = td->taskData;

    MStringArray uvSetNames;
    MStatus numPolygonsStat;
    MFloatArray uArray;
    MFloatArray vArray;
    for (unsigned int i = td->start; i < td->end; ++i) {
        const MDagPath& dagPath = taskData->meshArray[i];

        MFnMesh fnMesh(dagPath, &td->stat);
        CheckErrorReturnMThreadRetVal(td->stat, "searchMeshUVNegativeTd: could not create MFnMesh.");

        const int numPolygons = fnMesh.numPolygons(&numPolygonsStat);
        CheckErrorReturnMThreadRetVal(numPolygonsStat, "searchMeshUVNegativeTd: could not get num polygons.");

        if (numPolygons == 0) {
            // If this mesh don't have any face, skip it.
            MGlobal::displayWarning(dagPath.partialPathName() + " is zero polygon. skip.");
            continue;
        }

        uvSetNames.clear();
        td->stat = fnMesh.getUVSetNames(uvSetNames);
        CheckErrorReturnMThreadRetVal(td->stat, "searchMeshUVNegativeTd: could not get uv set names.");

        // check uv set names.
        if (uvSetNames.indexOf(taskData->uvSet) == -1 && !taskData->allUVSet) {
            // Skip if this mesh don't have the specify uvSet wanted to.
            MGlobal::displayWarning(dagPath.partialPathName() + " has't the " + taskData->uvSet + " uvSet. skip.");
            continue;
        }

        MItMeshPolygon itMeshPolygon(dagPath, MObject::kNullObj, &td->stat);
        CheckErrorReturnMThreadRetVal(td->stat, "searchMeshUVNegativeTd: could not create MItMeshPolygon.");

        for (; !itMeshPolygon.isDone(); itMeshPolygon.next()) {
            if (taskData->allUVSet) {
                for (unsigned int ii = 0; ii < uvSetNames.length(); ++ii) {
                    const MString& uvSetName = uvSetNames[ii];

                    uArray.clear();
                    vArray.clear();
                    td->stat = itMeshPolygon.getUVs(uArray, vArray, &uvSetName);
                    CheckErrorReturnMThreadRetVal(td->stat,
                        "searchMeshUVNegativeTd: could not get UVs.");

                    bool hasNegative = false;
                    for (unsigned int uv = 0; uv < uArray.length(); ++uv) {
                        if (uArray[uv] < 0.0f || vArray[uv] < 0.0f) {
                            hasNegative = true;
                            break;
                        }
                    }

                    if (hasNegative) {
                        MObject faceComponent = itMeshPolygon.currentItem(&td->stat);
                        CheckErrorReturnMThreadRetVal(td->stat,
                            "searchMeshUVNegativeTd: could not get face component.");

                        td->stat = td->invalidList.add(dagPath, faceComponent);
                        CheckErrorReturnMThreadRetVal(td->stat,
                            "searchMeshUVNegativeTd: could not add invalid list.");
                    }
                }
                continue;
            }

            uArray.clear();
            vArray.clear();
            td->stat = itMeshPolygon.getUVs(uArray, vArray, &taskData->uvSet);
            CheckErrorReturnMThreadRetVal(td->stat,
                "searchMeshUVNegativeTd: could not get UVs.");

            bool hasNegative = false;
            for (unsigned int uv = 0; uv < uArray.length(); ++uv) {
                if (uArray[uv] < 0.0f || vArray[uv] < 0.0f) {
                    hasNegative = true;
                    break;
                }
            }

            if (hasNegative) {
                MObject faceComponent = itMeshPolygon.currentItem(&td->stat);
                CheckErrorReturnMThreadRetVal(td->stat,
                    "searchMeshUVNegativeTd: could not get face component.");

                td->stat = td->invalidList.add(dagPath, faceComponent);
                CheckErrorReturnMThreadRetVal(td->stat,
                    "searchMeshUVNegativeTd: could not add invalid list.");
            }
        }
    }

    return (MThreadRetVal)0;
}

void searchMeshUVNegative(void* data, MThreadRootTask* root) {

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

    std::vector<SearchMeshUVNegativeTdData> threadData(size);

    float size_f = static_cast<float>(size);
    float meshLength_f = static_cast<float>(taskData->meshArray.size());

    for (unsigned int i = 0; i < size; ++i) {
        threadData[i].start = static_cast<unsigned int>(meshLength_f / size_f * static_cast<float>(i));
        threadData[i].end = static_cast<unsigned int>(meshLength_f / size_f * static_cast<float>(i + 1));
        threadData[i].taskData = taskData;
        threadData[i].stat = MStatus::kSuccess;

        MThreadPool::createTask(searchMeshUVNegativeTd, (void *)&threadData[i], root);
    }

    MThreadPool::executeAndJoin(root);

    for (unsigned int i = 0; i < size; ++i) {
        if (threadData[i].invalidList.length() > 0) {
            taskData->stat = taskData->invalidList.merge(threadData[i].invalidList);
            CheckErrorBreak(taskData->stat, "searchMeshUVNegative: could not merge invalid list");
        }

        taskData->stat = threadData[i].stat;
        CheckErrorBreak(taskData->stat, "searchMeshUVNegative: thread error");
    }
}

MStatus checkMeshUVNegative::doIt(const MArgList& args) {
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
    MThreadPool::newParallelRegion(searchMeshUVNegative, (void *)&taskData);
    CheckDisplayErrorRelease(taskData.stat, "doIt: countMeshes error.");

#ifdef _DEBUG
    cerr << "searchMeshUVNegative = " << timer.elapsed(&stat) << "sec.\n";
    CheckDisplayError(stat, "doIt: searchMeshUVNegative timer elapsed error.");
    stat = timer.restart();
    CheckDisplayError(stat, "doIt: searchMeshUVNegative timer reset error.");
#endif // _DEBUG

    _invalid = taskData.invalidList;

    stat = redoIt();

    return stat;
}

MStatus checkMeshUVNegative::redoIt() {
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

MStatus checkMeshUVNegative::undoIt() {
    if (_fIsSelect) {
        MStatus stat = MGlobal::setActiveSelectionList(_beforeSelection);
        return stat;
    }
    return MStatus::kSuccess;
}

bool checkMeshUVNegative::isUndoable() const {
    return true;
}

void* checkMeshUVNegative::creator() {
    return new checkMeshUVNegative();
}

MStatus initializePlugin(MObject obj)
{
    MFnPlugin plugin(obj, "nrtkbb", "1.0", "Any");
    plugin.registerCommand("checkMeshUVNegative",
        checkMeshUVNegative::creator, checkMeshUVNegative::createSyntax);
    return MS::kSuccess;
}
MStatus uninitializePlugin(MObject obj)
{
    MFnPlugin plugin( obj );
    plugin.deregisterCommand("checkMeshUVNegative");
    return MS::kSuccess;
}

