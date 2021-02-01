/*********************                                                        */
/*! \file SignConstraint.cpp
 ** \verbatim
 ** Top contributors (to current version):
 **   Guy Amir
 ** This file is part of the Marabou project.
 ** Copyright (c) 2017-2019 by the authors listed in the file AUTHORS
 ** in the top-level source directory) and their institutional affiliations.
 ** All rights reserved. See the file COPYING in the top-level source
 ** directory for licensing information.\endverbatim
 **
 ** [[ Add lengthier description here ]]
 **/

#include "Debug.h"
#include "FloatUtils.h"
#include "ITableau.h"
#include "MStringf.h"
#include "MarabouError.h"
#include "PiecewiseLinearCaseSplit.h"
#include "SignConstraint.h"
#include "Statistics.h"

#ifdef _WIN32
#define __attribute__(x)
#endif

SignConstraint::SignConstraint( unsigned b, unsigned f )
    : _b( b )
    , _f( f )
    , _direction( PHASE_NOT_FIXED )
    , _haveEliminatedVariables( false )
{
}

SignConstraint::SignConstraint( const String &serializedSign )
    : _haveEliminatedVariables( false )
{
    String constraintType = serializedSign.substring( 0, 4 );
    ASSERT( constraintType == String( "sign" ) );

    // Remove the constraint type in serialized form
    String serializedValues = serializedSign.substring( 5, serializedSign.length() - 5 );
    List<String> values = serializedValues.tokenize( "," );

    ASSERT( values.size() == 2 );

    auto var = values.begin();
    _f = atoi( var->ascii() );
    ++var;
    _b = atoi( var->ascii() );
}

PiecewiseLinearFunctionType SignConstraint::getType() const
{
    return PiecewiseLinearFunctionType::SIGN;
}

PiecewiseLinearConstraint *SignConstraint::duplicateConstraint() const
{
    SignConstraint *clone = new SignConstraint( _b, _f );
    *clone = *this;
    clone->reinitializeCDOs();
    return clone;
}

void SignConstraint::restoreState( const PiecewiseLinearConstraint *state )
{
    const SignConstraint *sign = dynamic_cast<const SignConstraint *>( state );
    *this = *sign;
}

void SignConstraint::registerAsWatcher( ITableau *tableau )
{
    tableau->registerToWatchVariable( this, _b );
    tableau->registerToWatchVariable( this, _f );
}

void SignConstraint::unregisterAsWatcher( ITableau *tableau )
{
    tableau->unregisterToWatchVariable( this, _b );
    tableau->unregisterToWatchVariable( this, _f );
}

bool SignConstraint::participatingVariable( unsigned variable ) const
{
    return ( variable == _b ) || ( variable == _f );
}

List<unsigned> SignConstraint::getParticipatingVariables() const
{
    return List<unsigned>( { _b, _f } );
}

bool SignConstraint::satisfied() const
{
    return false;
}

List<PiecewiseLinearCaseSplit> SignConstraint::getCaseSplits() const
{
    if ( *_phaseStatus != PHASE_NOT_FIXED )
        throw MarabouError( MarabouError::REQUESTED_CASE_SPLITS_FROM_FIXED_CONSTRAINT );

    List <PiecewiseLinearCaseSplit> splits;

    if ( _direction == SIGN_PHASE_NEGATIVE )
    {
      splits.append( getNegativeSplit() );
      splits.append( getPositiveSplit() );
      return splits;
    }
    if ( _direction == SIGN_PHASE_POSITIVE )
    {
      splits.append( getPositiveSplit() );
      splits.append( getNegativeSplit() );
      return splits;
    }

    // Default
    splits.append( getNegativeSplit() );
    splits.append( getPositiveSplit() );

    return splits;
}

PiecewiseLinearCaseSplit SignConstraint::getNegativeSplit() const
{
    // Negative phase: b < 0, f = -1
    PiecewiseLinearCaseSplit negativePhase;
    negativePhase.storeBoundTightening( Tightening( _b, 0.0, Tightening::UB ) );
    negativePhase.storeBoundTightening( Tightening( _f, -1.0, Tightening::UB ) );
    return negativePhase;
}

PiecewiseLinearCaseSplit SignConstraint::getPositiveSplit() const
{
    // Positive phase: b >= 0, f = 1
    PiecewiseLinearCaseSplit positivePhase;
    positivePhase.storeBoundTightening( Tightening( _b, 0.0, Tightening::LB ) );
    positivePhase.storeBoundTightening( Tightening( _f, 1.0, Tightening::LB ) );
    return positivePhase;
}

bool SignConstraint::phaseFixed() const
{
    return *_phaseStatus != PHASE_NOT_FIXED;
}

PiecewiseLinearCaseSplit SignConstraint::getValidCaseSplit() const
{
    ASSERT( *_phaseStatus != PHASE_NOT_FIXED );

    if ( *_phaseStatus == PhaseStatus::SIGN_PHASE_POSITIVE )
        return getPositiveSplit();

    return getNegativeSplit();
}

bool SignConstraint::constraintObsolete() const
{
    return _haveEliminatedVariables;
}

String SignConstraint::serializeToString() const
{
    // Output format is: sign,f,b
    return Stringf( "sign,%u,%u", _f, _b );
}

bool SignConstraint::haveOutOfBoundVariables() const
{
    return true;
}

String SignConstraint::phaseToString( PhaseStatus phase )
{
    switch ( phase )
    {
        case PHASE_NOT_FIXED:
            return "PHASE_NOT_FIXED";

        case SIGN_PHASE_POSITIVE:
            return "SIGN_PHASE_POSITIVE";

        case SIGN_PHASE_NEGATIVE:
            return "SIGN_PHASE_NEGATIVE";

        default:
            return "UNKNOWN";
    }
};

void SignConstraint::notifyLowerBound( unsigned variable, double bound )
{
    if ( _statistics )
        _statistics->incLongAttr( Statistics::NUM_CONSTRAINT_BOUND_TIGHTENING_ATTEMPT, 1 );

    // If there's an already-stored tighter bound, return
    if ( _lowerBounds.exists( variable ) && !FloatUtils::gt( bound, _lowerBounds[variable] ) )
        return;

    // Otherwise - update bound
    _lowerBounds[variable] = bound;

    if ( variable == _f && FloatUtils::gt( bound, -1 ) )
    {
        setPhaseStatus( PhaseStatus::SIGN_PHASE_POSITIVE );
        if ( _boundManager )
        {
            _boundManager->tightenLowerBound( _f, 1 );
            _boundManager->tightenLowerBound( _b, 0 );
        }
    }
    else if ( variable == _b && !FloatUtils::isNegative( bound ) )
    {
        setPhaseStatus( PhaseStatus::SIGN_PHASE_POSITIVE );
        if ( _boundManager )
        {
            _boundManager->tightenLowerBound( _f, 1 );
        }
    }
}

void SignConstraint::notifyUpperBound( unsigned variable, double bound )
{
    if ( _statistics )
        _statistics->incLongAttr( Statistics::NUM_CONSTRAINT_BOUND_TIGHTENING_ATTEMPT, 1 );

    // If there's an already-stored tighter bound, return
    if ( _upperBounds.exists( variable ) && !FloatUtils::lt( bound, _upperBounds[variable] ) )
        return;

    // Otherwise - update bound
    _upperBounds[variable] = bound;

    if ( variable == _f && FloatUtils::lt( bound, 1 ) )
    {
        setPhaseStatus( PhaseStatus::SIGN_PHASE_NEGATIVE );
        if ( _boundManager )
        {
            _boundManager->tightenUpperBound( _f, -1 );
            _boundManager->tightenUpperBound( _b, 0 );
        }
    }
    else if ( variable == _b && FloatUtils::isNegative( bound ) )
    {
        setPhaseStatus( PhaseStatus::SIGN_PHASE_NEGATIVE );
        if ( _boundManager )
        {
            _boundManager->tightenUpperBound( _f, -1 );
        }
    }
}

void SignConstraint::getEntailedTightenings( List<Tightening> &tightenings ) const
{
    ASSERT( _lowerBounds.exists( _b ) && _lowerBounds.exists( _f ) &&
            _upperBounds.exists( _b ) && _upperBounds.exists( _f ) );

    double bLowerBound = _lowerBounds[_b];
    double fLowerBound = _lowerBounds[_f];

    double bUpperBound = _upperBounds[_b];
    double fUpperBound = _upperBounds[_f];

    // Always make f between -1 and 1
    tightenings.append( Tightening( _f, -1, Tightening::LB ) );
    tightenings.append( Tightening( _f, 1, Tightening::UB ) );

    // Additional bounds can only be propagated if we are in the POSITIVE or NEGATIVE phases
    if ( !FloatUtils::isNegative( bLowerBound ) ||
         FloatUtils::gt( fLowerBound, -1 ) )
    {
        // Positive case
        tightenings.append( Tightening( _b, 0, Tightening::LB ) );
        tightenings.append( Tightening( _f, 1, Tightening::LB ) );
    }
    else if ( FloatUtils::isNegative( bUpperBound ) ||
              FloatUtils::lt( fUpperBound, 1 ) )
    {
        // Negative case
        tightenings.append( Tightening( _b, 0, Tightening::UB ) );
        tightenings.append( Tightening( _f, -1, Tightening::UB ) );
    }
}

void SignConstraint::setPhaseStatus( PhaseStatus phaseStatus )
{
    *_phaseStatus = phaseStatus;
}

void SignConstraint::updateVariableIndex( unsigned oldIndex, unsigned newIndex )
{
    ASSERT( oldIndex == _b || oldIndex == _f  );
    ASSERT( !_lowerBounds.exists( newIndex ) &&
            !_upperBounds.exists( newIndex ) &&
            newIndex != _b && newIndex != _f );

    if ( _lowerBounds.exists( oldIndex ) )
    {
        _lowerBounds[newIndex] = _lowerBounds.get( oldIndex );
        _lowerBounds.erase( oldIndex );
    }

    if ( _upperBounds.exists( oldIndex ) )
    {
        _upperBounds[newIndex] = _upperBounds.get( oldIndex );
        _upperBounds.erase( oldIndex );
    }

    if ( oldIndex == _b )
        _b = newIndex;
    else if ( oldIndex == _f )
        _f = newIndex;
}

void SignConstraint::eliminateVariable( __attribute__((unused)) unsigned variable,
                                        __attribute__((unused)) double fixedValue )
{
    ASSERT( variable == _b || variable == _f );

    DEBUG({
              if ( variable == _f )
              {
                  ASSERT( ( FloatUtils::areEqual( fixedValue, 1 ) ) ||
                          ( FloatUtils::areEqual( fixedValue,-1 ) ) );

                  if ( FloatUtils::areEqual( fixedValue, 1 ) )
                  {
                      ASSERT( *_phaseStatus != SIGN_PHASE_NEGATIVE );
                  }
                  else if (FloatUtils::areEqual( fixedValue, -1 ) )
                  {
                      ASSERT( *_phaseStatus != SIGN_PHASE_POSITIVE );
                  }
              }
              else if ( variable == _b )
              {
                  if ( FloatUtils::gte( fixedValue, 0 ) )
                  {
                      ASSERT( *_phaseStatus != SIGN_PHASE_NEGATIVE );
                  }
                  else if ( FloatUtils::lt( fixedValue, 0 ) )
                  {
                      ASSERT( *_phaseStatus != SIGN_PHASE_POSITIVE );
                  }
              }
        });

    // In a Sign constraint, if a variable is removed the entire constraint can be discarded.
    _haveEliminatedVariables = true;
}

unsigned SignConstraint::getB() const
{
    return _b;
}

unsigned SignConstraint::getF() const
{
    return _f;
}

void SignConstraint::dump( String &output ) const
{
    PhaseStatus phase = *_phaseStatus;
    output = Stringf( "SignConstraint: x%u = Sign( x%u ). Active? %s. PhaseStatus = %u (%s). ",
                      _f, _b,
                      *_constraintActive ? "Yes" : "No",
                      phase, phaseToString( phase ).ascii()
                      );

    output += Stringf( "b in [%s, %s], ",
                       _lowerBounds.exists( _b ) ? Stringf( "%lf", _lowerBounds[_b] ).ascii() : "-inf",
                       _upperBounds.exists( _b ) ? Stringf( "%lf", _upperBounds[_b] ).ascii() : "inf" );

    output += Stringf( "f in [%s, %s]\n",
                       _lowerBounds.exists( _f ) ? Stringf( "%lf", _lowerBounds[_f] ).ascii() : "-inf",
                       _upperBounds.exists( _f ) ? Stringf( "%lf", _upperBounds[_f] ).ascii() : "inf" );
}

double SignConstraint::computePolarity() const
{
  double currentLb = _lowerBounds[_b];
  double currentUb = _upperBounds[_b];
  if ( !FloatUtils::isNegative( currentLb ) ) return 1;
  if ( FloatUtils::isNegative( currentUb ) ) return -1;
  double width = currentUb - currentLb;
  double sum = currentUb + currentLb;
  return sum / width;
}

void SignConstraint::updateDirection()
{
    _direction = ( FloatUtils::isNegative( computePolarity() ) ) ?
        SIGN_PHASE_NEGATIVE : SIGN_PHASE_POSITIVE;
}

PhaseStatus SignConstraint::getDirection() const
{
  return _direction;
}

void SignConstraint::updateScoreBasedOnPolarity()
{
  _score = std::abs( computePolarity() );
}

bool SignConstraint::supportPolarity() const
{
  return true;
}