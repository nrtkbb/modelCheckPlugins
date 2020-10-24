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
#include <tuple>
#include <map>
#include <maya/MFn.h>
#include <maya/MFnPlugin.h>
#include <maya/MFnDagNode.h>
#include <maya/MFnNurbsCurve.h>
#include <maya/MItDag.h>
#include <maya/MPxCommand.h>
#include <maya/MGlobal.h>
#include <maya/MSyntax.h>
#include <maya/MDagPath.h>
#include <maya/MArgList.h>
#include <maya/MArgParser.h>
#include <maya/MPointArray.h>
#include <maya/MSelectionList.h>


class checkCurveSpans0Count : public MPxCommand
{
    public:
        checkCurveSpans0Count();
        virtual ~checkCurveSpans0Count();
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

checkCurveSpans0Count::checkCurveSpans0Count() {
}

checkCurveSpans0Count::~checkCurveSpans0Count() {
}

MSyntax checkCurveSpans0Count::createSyntax() {
    MSyntax syntax;

    syntax.addFlag("-s", "-select", MSyntax::kNoArg);
    return syntax;
}

MStatus checkCurveSpans0Count::doIt(const MArgList& args) {
    MStatus stat = MStatus::kSuccess;

    MArgParser argData(syntax(), args, &stat);

    if (argData.isFlagSet("select")) {
        _isSelect = true;
        MGlobal::getActiveSelectionList(_beforeSelection);
    }
    else {
        _isSelect = false;
    }

    MItDag dagIter(MItDag::kDepthFirst, MFn::kNurbsCurve, &stat);
    CHECK_MSTATUS_AND_RETURN_IT(stat);

    for ( ; !dagIter.isDone(); dagIter.next()) {
        MDagPath dagPath;
        stat = dagIter.getPath(dagPath);
        CHECK_MSTATUS_AND_RETURN_IT(stat);

        MFnDagNode dagNode(dagPath, &stat);
        CHECK_MSTATUS_AND_RETURN_IT(stat);

        if (dagNode.isIntermediateObject()) {
            continue;
        }

        if (!dagPath.hasFn(MFn::kNurbsCurve) || dagPath.hasFn(MFn::kTransform)) {
            continue;
        }

        MFnNurbsCurve fnCurve(dagPath, &stat);
        if (stat != MStatus::kSuccess) {
            stat = _invalid.add(dagPath);
            CHECK_MSTATUS_AND_RETURN_IT(stat);

            continue;
        }

        const int numSpans = fnCurve.numSpans(&stat);
        CHECK_MSTATUS_AND_RETURN_IT(stat);

        if (numSpans == 0) {
            stat = _invalid.add(dagPath);
            CHECK_MSTATUS_AND_RETURN_IT(stat);
        }
    }
    stat = redoIt();

    return stat;
}

MStatus checkCurveSpans0Count::redoIt() {
    if (_isSelect) {
        MStatus stat = MGlobal::setActiveSelectionList(_invalid);
        return stat;
    }
    MStringArray results;
    MStatus stat = _invalid.getSelectionStrings(results);
    CHECK_MSTATUS_AND_RETURN_IT(stat);

    setResult(results);
    return stat;
}

MStatus checkCurveSpans0Count::undoIt() {
    if (_isSelect) {
        MStatus stat = MGlobal::setActiveSelectionList(_beforeSelection);
        return stat;
    }
    return MStatus::kSuccess;
}

bool checkCurveSpans0Count::isUndoable() const {
    return true;
}

void* checkCurveSpans0Count::creator() {
    return new checkCurveSpans0Count();
}

MStatus initializePlugin(MObject obj)
{
    MFnPlugin plugin(obj, "nrtkbb", "1.0", "Any");
    plugin.registerCommand("checkCurveSpans0Count",
            checkCurveSpans0Count::creator, checkCurveSpans0Count::createSyntax);
    return MS::kSuccess;
}
MStatus uninitializePlugin(MObject obj)
{
    MFnPlugin plugin( obj );
    plugin.deregisterCommand("checkCurveSpans0Count");
    return MS::kSuccess;
}

