/*---------------------------------------------------------------------------*\

    DAFoam  : Discrete Adjoint with OpenFOAM
    Version : v2

\*---------------------------------------------------------------------------*/

#include "DAFvSourceActuatorLine.H"

// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

namespace Foam
{

defineTypeNameAndDebug(DAFvSourceActuatorLine, 0);
addToRunTimeSelectionTable(DAFvSource, DAFvSourceActuatorLine, dictionary);
// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

DAFvSourceActuatorLine::DAFvSourceActuatorLine(
    const word modelType,
    const fvMesh& mesh,
    const DAOption& daOption,
    const DAModel& daModel,
    const DAIndex& daIndex)
    : DAFvSource(modelType, mesh, daOption, daModel, daIndex)
{
    printIntervalUnsteady_ = daOption.getOption<label>("printIntervalUnsteady");
}
// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

void DAFvSourceActuatorLine::calcFvSource(volVectorField& fvSource)
{
    /*
    Description:
        Compute the actuator line source term. 
        Reference:  Stokkermans et al. Validation and comparison of RANS propeller 
        modeling methods for tip-mounted applications
    */

    const scalar& pi = Foam::constant::mathematical::pi;

    scalar t = mesh_.time().timeOutputValue();

    forAll(fvSource, idxI)
    {
        fvSource[idxI] = vector::zero;
    }

    dictionary fvSourceSubDict = daOption_.getAllOptions().subDict("fvSource");

    // loop over all the cell indices for all actuator lines
    forAll(fvSourceSubDict.toc(), idxI)
    {
        // name of this line model
        word lineName = fvSourceSubDict.toc()[idxI];

        // sub dictionary with all parameters for this disk
        dictionary lineSubDict = fvSourceSubDict.subDict(lineName);

        // now read in all parameters for this actuator line
        // center of the actuator line
        scalarList center1;
        lineSubDict.readEntry<scalarList>("center", center1);
        vector center = {center1[0], center1[1], center1[2]};
        // thrust direction
        scalarList direction1;
        lineSubDict.readEntry<scalarList>("direction", direction1);
        vector direction = {direction1[0], direction1[1], direction1[2]};
        direction = direction / mag(direction);
        // initial vector for the actuator line
        scalarList initial1;
        lineSubDict.readEntry<scalarList>("initial", initial1);
        vector initial = {initial1[0], initial1[1], initial1[2]};
        initial = initial / mag(initial);
        if (fabs(direction & initial) > 1.0e-10)
        {
            FatalErrorIn(" ") << "direction and initial need to be orthogonal!" << abort(FatalError);
        }
        // rotation direction, can be either right or left
        word rotDir = lineSubDict.getWord("rotDir");
        // inner and outer radius of the lines
        scalar innerRadius = lineSubDict.get<scalar>("innerRadius");
        scalar outerRadius = lineSubDict.get<scalar>("outerRadius");
        // rotation speed in rpm
        scalar rpm = lineSubDict.get<scalar>("rpm");
        scalar radPerS = rpm / 60.0 * 2.0 * pi;
        // smooth factor which should be of grid size
        scalar eps = lineSubDict.get<scalar>("eps");
        // scaling factor to ensure a desired integrated thrust
        scalar scale = lineSubDict.get<scalar>("scale");
        // phase (rad) of the rotation positive value means rotates ahead of time for phase rad
        scalar phase = lineSubDict.get<scalar>("phase");
        // number of blades for this line model
        label nBlades = lineSubDict.get<label>("nBlades");
        scalar POD = lineSubDict.getScalar("POD");
        scalar expM = lineSubDict.getScalar("expM");
        scalar expN = lineSubDict.getScalar("expN");
        scalar rStarMin = lineSubDict.lookupOrDefault<scalar>("rStarMin", 0.02);
        scalar rStarMax = lineSubDict.lookupOrDefault<scalar>("rStarMax", 0.98);
        scalar epsR = lineSubDict.lookupOrDefault<scalar>("epsR", 0.02);
        scalar fRMin = Foam::pow(rStarMin, expM) * Foam::pow(1.0 - rStarMin, expN);
        scalar fRMax = Foam::pow(rStarMax, expM) * Foam::pow(1.0 - rStarMax, expN);

        scalar thrustTotal = 0.0;
        scalar torqueTotal = 0.0;
        forAll(mesh_.C(), cellI)
        {
            // the cell center coordinates of this cellI
            vector cellC = mesh_.C()[cellI];
            // cell center to disk center vector
            vector cellC2AVec = cellC - center;
            // tmp tensor for calculating the axial/radial components of cellC2AVec
            tensor cellC2AVecE(tensor::zero);
            cellC2AVecE.xx() = cellC2AVec.x();
            cellC2AVecE.yy() = cellC2AVec.y();
            cellC2AVecE.zz() = cellC2AVec.z();
            // now we need to decompose cellC2AVec into axial and radial components
            // the axial component of cellC2AVec vector
            vector cellC2AVecA = cellC2AVecE & direction;
            // the radial component of cellC2AVec vector
            vector cellC2AVecR = cellC2AVec - cellC2AVecA;
            // now we can use the cross product to compute the tangential
            // (circ) direction of cellI
            vector cellC2AVecC(vector::zero);
            if (rotDir == "left")
            {
                // this assumes right hand rotation of propellers
                cellC2AVecC = cellC2AVecR ^ cellC2AVecA; // circ
            }
            else if (rotDir == "right")
            {
                // this assumes left hand rotation of propellers
                cellC2AVecC = cellC2AVecA ^ cellC2AVecR; // circ
            }
            else
            {
                FatalErrorIn(" ") << "rotDir not valid" << abort(FatalError);
            }
            // the magnitude of radial component of cellC2AVecR
            scalar cellC2AVecRLen = mag(cellC2AVecR);
            // the magnitude of tangential component of cellC2AVecR
            scalar cellC2AVecCLen = mag(cellC2AVecC);
            // the magnitude of axial component of cellC2AVecA
            scalar cellC2AVecALen = mag(cellC2AVecA);
            // the normalized cellC2AVecC (tangential) vector
            vector cellC2AVecCNorm = cellC2AVecC / (cellC2AVecCLen + SMALL);
            // smooth coefficient in the axial direction
            scalar etaAxial = Foam::exp(-Foam::sqr(cellC2AVecALen / eps)) / Foam::exp(0.0);

            scalar etaTheta = 0.0;
            for (label bb = 0; bb < nBlades; bb++)
            {
                scalar thetaBlade = bb * 2.0 * pi / nBlades + radPerS * t + phase;
                if (daOption_.getOption<word>("runStatus") == "solvePrimal")
                {
                    if (cellI == 0)
                    {
                        if (mesh_.time().timeIndex() % printIntervalUnsteady_ == 0
                            || mesh_.time().timeIndex() == 1)
                        {
                            Info << "blade " << bb << " theta: "
                                 << fmod(thetaBlade, 2.0 * pi) * 180.0 / pi
                                 << " deg" << endl;
                        }
                    }
                }
                // compute the rotated vector of initial by thetaBlade degree
                // We use a simplified version of Rodrigues rotation formulation
                vector rotatedVec = vector::zero;
                if (rotDir == "right")
                {
                    rotatedVec = initial * Foam::cos(thetaBlade)
                        + (direction ^ initial) * Foam::sin(thetaBlade);
                }
                else if (rotDir == "left")
                {
                    rotatedVec = initial * Foam::cos(thetaBlade)
                        + (initial ^ direction) * Foam::sin(thetaBlade);
                }
                else
                {
                    FatalErrorIn(" ") << "rotDir not valid" << abort(FatalError);
                }
                // scale the rotated vector to have the same length as cellC2AVecR
                rotatedVec *= cellC2AVecRLen;
                // now we can compute the distance between the cellC2AVecR and the rotatedVec
                scalar dS_Theta = mag(cellC2AVecR - rotatedVec);
                // smooth coefficient in the theta direction
                etaTheta += Foam::exp(-Foam::sqr(dS_Theta / eps)) / Foam::exp(0.0);
            }

            // now we can use Hoekstra's formulation to compute radial thrust distribution
            scalar rPrime = cellC2AVecRLen / outerRadius;
            scalar rPrimeHub = innerRadius / outerRadius;
            // rStar is normalized radial location
            scalar rStar = (rPrime - rPrimeHub) / (1.0 - rPrimeHub);

            scalar fAxial = 0.0;
            scalar fCirc = 0.0;
            scalar rPrimeMin = rStarMin * (1.0 - rPrimeHub) + rPrimeHub;
            scalar rPrimeMax = rStarMax * (1.0 - rPrimeHub) + rPrimeHub;
            if (rStar < rStarMin)
            {
                scalar dR2 = (rStar - rStarMin) * (rStar - rStarMin);
                fAxial = fRMin * Foam::exp(-dR2 / epsR / epsR) / Foam::exp(0.0);
                fCirc = fAxial * POD / pi / rPrimeMin;
            }
            else if (rStar >= rStarMin && rStar <= rStarMax)
            {
                fAxial = Foam::pow(rStar, expM) * Foam::pow(1.0 - rStar, expN);
                // we use Hoekstra's method to calculate the fCirc based on fAxial
                fCirc = fAxial * POD / pi / rPrime;
            }
            else
            {
                scalar dR2 = (rStar - rStarMax) * (rStar - rStarMax);
                fAxial = fRMax * Foam::exp(-dR2 / epsR / epsR) / Foam::exp(0.0);
                fCirc = fAxial * POD / pi / rPrimeMax;
            }

            vector sourceVec = (fAxial * direction + fCirc * cellC2AVecCNorm);

            fvSource[cellI] += scale * etaAxial * etaTheta * sourceVec;

            thrustTotal += scale * etaAxial * etaTheta * fAxial * mesh_.V()[cellI];
            torqueTotal += scale * etaAxial * etaTheta * fCirc * mesh_.V()[cellI];
        }
        reduce(thrustTotal, sumOp<scalar>());
        reduce(torqueTotal, sumOp<scalar>());

        if (daOption_.getOption<word>("runStatus") == "solvePrimal")
        {
            if (mesh_.time().timeIndex() % printIntervalUnsteady_ == 0 || mesh_.time().timeIndex() == 1)
            {
                Info << "Actuator line source: " << lineName << endl;
                Info << "Total thrust source: " << thrustTotal << endl;
                Info << "Total torque source: " << torqueTotal << endl;
            }
        }
    }
}

} // End namespace Foam

// ************************************************************************* //
