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

#define CheckDisplayError(STAT,MSG)    \
    if ( MStatus::kSuccess != STAT ) { \
        MGlobal::displayError(MSG);\
        return MStatus::kFailure;      \
    }


class checkCurveSamePosition : public MPxCommand
{
    public:
        checkCurveSamePosition();
        virtual ~checkCurveSamePosition();
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

checkCurveSamePosition::checkCurveSamePosition() {
}
checkCurveSamePosition::~checkCurveSamePosition() {
}

MSyntax checkCurveSamePosition::createSyntax() {
    MSyntax syntax;

    syntax.addFlag("-s", "-select", MSyntax::kNoArg);
    return syntax;
}

MStatus checkCurveSamePosition::doIt(const MArgList& args) {
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
    CheckDisplayError(stat, "doIt: could not create dagIter.\n");

    std::map<std::tuple<double, double, double>, MDagPath> check_map{};
    MPointArray cvPositions;
    for ( ; !dagIter.isDone(); dagIter.next()) {
        MDagPath dagPath;
        stat = dagIter.getPath(dagPath);
        CheckDisplayError(stat, "doIt: could not get dag path.\n");

        MFnDagNode dagNode(dagPath, &stat);
        CheckDisplayError(stat, "doIt: could not get dag node.\n");

        if (dagNode.isIntermediateObject()) {
            continue;
        }

        if (!dagPath.hasFn(MFn::kNurbsCurve) || dagPath.hasFn(MFn::kTransform)) {
            continue;
        }

        MFnNurbsCurve fnCurve(dagPath, &stat);
        CheckDisplayError(stat, "doIt: could not create MFnNurbsCurve.\n");

        stat = fnCurve.getCVs(cvPositions, MSpace::kWorld);
        CheckDisplayError(stat, "doIt: could not get cv positions.\n");

        unsigned int numCVs = cvPositions.length();
        MFnNurbsCurve::Form form = fnCurve.form(&stat);
        CheckDisplayError(stat, "doIt: could not get form.\n");

        if (form == MFnNurbsCurve::kPeriodic) {
            numCVs -= 3;
        }

        for (unsigned int i = 0; i < numCVs; i++) {

            auto cvPosition = cvPositions[i];
            std::tuple<double, double, double> cvp = 
                std::make_tuple(cvPosition.x, cvPosition.y, cvPosition.z);
            if (check_map.count(cvp) == 0) {
                check_map[cvp] = dagPath;
            } else {
                bool hasItem = _invalid.hasItem(dagPath, MObject::kNullObj, &stat);
                CheckDisplayError(stat, "doIt: could not get hasItem1.\n");
                if (!hasItem) {
                    stat = _invalid.add(dagPath);
                    CheckDisplayError(stat, "doIt: could not add invalid curve1.\n");
                }

                hasItem = _invalid.hasItem(check_map[cvp], MObject::kNullObj, &stat);
                CheckDisplayError(stat, "doIt: could not get hasItem2.\n");
                if (!hasItem) {
                    stat = _invalid.add(check_map[cvp]);
                    CheckDisplayError(stat, "doIt: could not add invalid curve2.\n");
                }
            }
        }
    }
    stat = redoIt();

    return stat;
}

MStatus checkCurveSamePosition::redoIt() {
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

MStatus checkCurveSamePosition::undoIt() {
    if (_isSelect) {
        MStatus stat = MGlobal::setActiveSelectionList(_beforeSelection);
        return stat;
    }
    return MStatus::kSuccess;
}

bool checkCurveSamePosition::isUndoable() const {
    return true;
}

void* checkCurveSamePosition::creator() {
    return new checkCurveSamePosition();
}

MStatus initializePlugin(MObject obj)
{
    MFnPlugin plugin(obj, "nrtkbb", "1.0", "Any");
    plugin.registerCommand("checkCurveSamePosition",
            checkCurveSamePosition::creator, checkCurveSamePosition::createSyntax);
    return MS::kSuccess;
}
MStatus uninitializePlugin(MObject obj)
{
    MFnPlugin plugin( obj );
    plugin.deregisterCommand("checkCurveSamePosition");
    return MS::kSuccess;
}

