/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | Copyright (C) 2011 OpenFOAM Foundation
     \\/     M anipulation  |
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

Description
    Cell layer addition/removal mesh modifier

\*---------------------------------------------------------------------------*/

#include "layerAdditionRemoval.H"
#include "polyTopoChanger.H"
#include "polyMesh.H"
#include "Time.H"
#include "primitiveMesh.H"
#include "polyTopoChange.H"
#include "addToRunTimeSelectionTable.H"

// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //

namespace Foam
{
    defineTypeNameAndDebug(layerAdditionRemoval, 0);
    addToRunTimeSelectionTable
    (
        polyMeshModifier,
        layerAdditionRemoval,
        dictionary
    );
}


const Foam::scalar Foam::layerAdditionRemoval::addDelta_ = 0.3;
const Foam::scalar Foam::layerAdditionRemoval::removeDelta_ = 0.1;


// * * * * * * * * * * * * * Private Member Functions  * * * * * * * * * * * //

void Foam::layerAdditionRemoval::checkDefinition()
{
    if (!faceZoneID_.active())
    {
        FatalErrorIn
        (
            "void Foam::layerAdditionRemoval::checkDefinition()"
        )   << "Master face zone named " << faceZoneID_.name()
            << " cannot be found."
            << abort(FatalError);
    }

    if
    (
        minLayerThickness_ < VSMALL
     || maxLayerThickness_ < minLayerThickness_
    )
    {
        FatalErrorIn
        (
            "void Foam::layerAdditionRemoval::checkDefinition()"
        )   << "Incorrect layer thickness definition."
            << abort(FatalError);
    }

    if (topoChanger().mesh().faceZones()[faceZoneID_.index()].empty())
    {
        FatalErrorIn
        (
            "void Foam::layerAdditionRemoval::checkDefinition()"
        )   << "Face extrusion zone contains no faces. "
            << " Please check your mesh definition."
            << abort(FatalError);
    }

    if (debug)
    {
        Pout<< "Cell layer addition/removal object " << name() << " :" << nl
            << "    faceZoneID: " << faceZoneID_ << endl;
    }
}

Foam::scalar Foam::layerAdditionRemoval::readOldThickness
(
    const dictionary& dict
)
{
    return dict.lookupOrDefault("oldLayerThickness", -1.0);
}


void Foam::layerAdditionRemoval::clearAddressing() const
{
    // Layer removal data
    deleteDemandDrivenData(pointsPairingPtr_);
    deleteDemandDrivenData(facesPairingPtr_);
}


// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

// Construct from components
Foam::layerAdditionRemoval::layerAdditionRemoval
(
    const word& name,
    const label index,
    const polyTopoChanger& mme,
    const word& zoneName,
    const scalar minThickness,
    const scalar maxThickness
)
:
    polyMeshModifier(name, index, mme, true),
    faceZoneID_(zoneName, mme.mesh().faceZones()),
    minLayerThickness_(minThickness),
    maxLayerThickness_(maxThickness),
    oldLayerThickness_(-1.0),
    pointsPairingPtr_(NULL),
    facesPairingPtr_(NULL),
    triggerRemoval_(-1),
    triggerAddition_(-1)
{
    checkDefinition();
}


// Construct from dictionary
Foam::layerAdditionRemoval::layerAdditionRemoval
(
    const word& name,
    const dictionary& dict,
    const label index,
    const polyTopoChanger& mme
)
:
    polyMeshModifier(name, index, mme, Switch(dict.lookup("active"))),
    faceZoneID_(dict.lookup("faceZoneName"), mme.mesh().faceZones()),
    minLayerThickness_(readScalar(dict.lookup("minLayerThickness"))),
    maxLayerThickness_(readScalar(dict.lookup("maxLayerThickness"))),
    oldLayerThickness_(readOldThickness(dict)),
    pointsPairingPtr_(NULL),
    facesPairingPtr_(NULL),
    triggerRemoval_(-1),
    triggerAddition_(-1)
{
    checkDefinition();
}


// * * * * * * * * * * * * * * * * Destructor  * * * * * * * * * * * * * * * //

Foam::layerAdditionRemoval::~layerAdditionRemoval()
{
    clearAddressing();
}


// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //

bool Foam::layerAdditionRemoval::changeTopology() const
{
    // Protect from multiple calculation in the same time-step
    if (triggerRemoval_ > -1 || triggerAddition_ > -1)
    {
        return true;
    }

    // Go through all the cells in the master layer and calculate
    // approximate layer thickness as the ratio of the cell volume and
    // face area in the face zone.
    // Layer addition:
    //     When the max thickness exceeds the threshold, trigger refinement.
    // Layer removal:
    //     When the min thickness falls below the threshold, trigger removal.

    const faceZone& fz = topoChanger().mesh().faceZones()[faceZoneID_.index()];
    const labelList& mc = fz.masterCells();

    const scalarField& V = topoChanger().mesh().cellVolumes();
    const vectorField& S = topoChanger().mesh().faceAreas();

    if (min(V) < -VSMALL)
    {
        FatalErrorIn("bool layerAdditionRemoval::changeTopology() const")
            << "negative cell volume. Error in mesh motion before "
            << "topological change.\n V: " << V
            << abort(FatalError);
    }

    scalar avgDelta = 0;
    scalar minDelta = GREAT;
    scalar maxDelta = 0;

    forAll(fz, faceI)
    {
        scalar curDelta = V[mc[faceI]]/mag(S[fz[faceI]]);
        avgDelta += curDelta;
        minDelta = min(minDelta, curDelta);
        maxDelta = max(maxDelta, curDelta);
    }

    avgDelta /= fz.size();

    ////MJ Alternative thickness determination
    //{
    //    // Edges on layer.
    //    const Map<label>& zoneMeshPointMap = fz().meshPointMap();
    //
    //    label nDelta = 0;
    //
    //    // Edges with only one point on zone
    //    const polyMesh& mesh = topoChanger().mesh();
    //
    //    forAll(mc, faceI)
    //    {
    //        const cell& cFaces = mesh.cells()[mc[faceI]];
    //        const edgeList cellEdges(cFaces.edges(mesh.faces()));
    //
    //        forAll(cellEdges, i)
    //        {
    //            const edge& e = cellEdges[i];
    //
    //            if (zoneMeshPointMap.found(e[0]))
    //            {
    //                if (!zoneMeshPointMap.found(e[1]))
    //                {
    //                    scalar curDelta = e.mag(mesh.points());
    //                    avgDelta += curDelta;
    //                    nDelta++;
    //                    minDelta = min(minDelta, curDelta);
    //                    maxDelta = max(maxDelta, curDelta);
    //                }
    //            }
    //            else
    //            {
    //                if (zoneMeshPointMap.found(e[1]))
    //                {
    //                    scalar curDelta = e.mag(mesh.points());
    //                    avgDelta += curDelta;
    //                    nDelta++;
    //                    minDelta = min(minDelta, curDelta);
    //                    maxDelta = max(maxDelta, curDelta);
    //                }
    //            }
    //        }
    //    }
    //
    //    avgDelta /= nDelta;
    //}

    if (debug)
    {
        Pout<< "bool layerAdditionRemoval::changeTopology() const "
            << " for object " << name() << " : " << nl
            << "Layer thickness: min: " << minDelta
            << " max: " << maxDelta << " avg: " << avgDelta
            << " old thickness: " << oldLayerThickness_ << nl
            << "Removal threshold: " << minLayerThickness_
            << " addition threshold: " << maxLayerThickness_ << endl;
    }

    bool topologicalChange = false;

    // If the thickness is decreasing and crosses the min thickness,
    // trigger removal
    if (oldLayerThickness_ < 0)
    {
        if (debug)
        {
            Pout<< "First step. No addition/removal" << endl;
        }

        // No topological changes allowed before first mesh motion
        //
        oldLayerThickness_ = avgDelta;

        topologicalChange = false;
    }
    else if (avgDelta < oldLayerThickness_)
    {
        // Layers moving towards removal
        if (minDelta < minLayerThickness_)
        {
            // Check layer pairing
            if (setLayerPairing())
            {
                // A mesh layer detected.  Check that collapse is valid
                if (validCollapse())
                {
                    // At this point, info about moving the old mesh
                    // in a way to collapse the cells in the removed
                    // layer is available.  Not sure what to do with
                    // it.

                    if (debug)
                    {
                        Pout<< "bool layerAdditionRemoval::changeTopology() "
                            << " const for object " << name() << " : "
                            << "Triggering layer removal" << endl;
                    }

                    triggerRemoval_ = topoChanger().mesh().time().timeIndex();

                    // Old thickness looses meaning.
                    // Set it up to indicate layer removal
                    oldLayerThickness_ = GREAT;

                    topologicalChange = true;
                }
                else
                {
                    // No removal, clear addressing
                    clearAddressing();
                }
            }
        }
        else
        {
            oldLayerThickness_ = avgDelta;
        }
    }
    else
    {
        // Layers moving towards addition
        if (maxDelta > maxLayerThickness_)
        {
            if (debug)
            {
                Pout<< "bool layerAdditionRemoval::changeTopology() const "
                    << " for object " << name() << " : "
                    << "Triggering layer addition" << endl;
            }

            triggerAddition_ = topoChanger().mesh().time().timeIndex();

            // Old thickness looses meaning.
            // Set it up to indicate layer removal
            oldLayerThickness_ = 0;

            topologicalChange = true;
        }
        else
        {
            oldLayerThickness_ = avgDelta;
        }
    }

    return topologicalChange;
}


void Foam::layerAdditionRemoval::setRefinement(polyTopoChange& ref) const
{
    // Insert the layer addition/removal instructions
    // into the topological change

    if (triggerRemoval_ == topoChanger().mesh().time().timeIndex())
    {
        removeCellLayer(ref);

        // Clear addressing.  This also resets the addition/removal data
        if (debug)
        {
            Pout<< "layerAdditionRemoval::setRefinement(polyTopoChange& ref) "
                << " for object " << name() << " : "
                << "Clearing addressing after layer removal. " << endl;
        }

        triggerRemoval_ = -1;
        clearAddressing();
    }

    if (triggerAddition_ == topoChanger().mesh().time().timeIndex())
    {
        addCellLayer(ref);

        // Clear addressing.  This also resets the addition/removal data
        if (debug)
        {
            Pout<< "layerAdditionRemoval::setRefinement(polyTopoChange& ref) "
                << " for object " << name() << " : "
                << "Clearing addressing after layer addition. " << endl;
        }

        triggerAddition_ = -1;
        clearAddressing();
    }
}


void Foam::layerAdditionRemoval::updateMesh(const mapPolyMesh&)
{
    if (debug)
    {
        Pout<< "layerAdditionRemoval::updateMesh(const mapPolyMesh&) "
            << " for object " << name() << " : "
            << "Clearing addressing on external request. ";

        if (pointsPairingPtr_ || facesPairingPtr_)
        {
            Pout<< "Pointers set." << endl;
        }
        else
        {
            Pout<< "Pointers not set." << endl;
        }
    }

    // Mesh has changed topologically.  Update local topological data
    faceZoneID_.update(topoChanger().mesh().faceZones());

    clearAddressing();
}


void Foam::layerAdditionRemoval::setMinLayerThickness(const scalar t) const
{
    if
    (
        t < VSMALL
     || maxLayerThickness_ < t
    )
    {
        FatalErrorIn
        (
            "void layerAdditionRemoval::setMinLayerThickness("
            "const scalar t) const"
        )   << "Incorrect layer thickness definition."
            << abort(FatalError);
    }

    minLayerThickness_ = t;
}


void Foam::layerAdditionRemoval::setMaxLayerThickness(const scalar t) const
{
    if
    (
        t < minLayerThickness_
    )
    {
        FatalErrorIn
        (
            "void layerAdditionRemoval::setMaxLayerThickness("
            "const scalar t) const"
        )   << "Incorrect layer thickness definition."
            << abort(FatalError);
    }

    maxLayerThickness_ = t;
}


void Foam::layerAdditionRemoval::write(Ostream& os) const
{
    os  << nl << type() << nl
        << name()<< nl
        << faceZoneID_ << nl
        << minLayerThickness_ << nl
        << oldLayerThickness_ << nl
        << maxLayerThickness_ << endl;
}


void Foam::layerAdditionRemoval::writeDict(Ostream& os) const
{
    os  << nl << name() << nl << token::BEGIN_BLOCK << nl
        << "    type " << type()
        << token::END_STATEMENT << nl
        << "    faceZoneName " << faceZoneID_.name()
        << token::END_STATEMENT << nl
        << "    minLayerThickness " << minLayerThickness_
        << token::END_STATEMENT << nl
        << "    maxLayerThickness " << maxLayerThickness_
        << token::END_STATEMENT << nl
        << "    oldLayerThickness " << oldLayerThickness_
        << token::END_STATEMENT << nl
        << "    active " << active()
        << token::END_STATEMENT << nl
        << token::END_BLOCK << endl;
}


// ************************************************************************* //
