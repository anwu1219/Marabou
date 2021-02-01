/*********************                                                        */
/*! \file RowBoundTightener.cpp
 ** \verbatim
 ** Top contributors (to current version):
 **   Guy Katz
 ** This file is part of the Marabou project.
 ** Copyright (c) 2017-2019 by the authors listed in the file AUTHORS
 ** in the top-level source directory) and their institutional affiliations.
 ** All rights reserved. See the file COPYING in the top-level source
 ** directory for licensing information.\endverbatim
 **
 ** [[ Add lengthier description here ]]

 **/

#include "Debug.h"
#include "InfeasibleQueryException.h"
#include "MarabouError.h"
#include "RowBoundTightener.h"
#include "SparseUnsortedList.h"
#include "Statistics.h"

RowBoundTightener::RowBoundTightener( const ITableau &tableau, BoundManager &boundManager )
    : _tableau( tableau )
    , _boundManager( boundManager )
    , _rows( NULL )
    , _z( NULL )
    , _ciTimesLb( NULL )
    , _ciTimesUb( NULL )
    , _ciSign( NULL )
    , _statistics( NULL )
{
    _boundManager.getSize();
}

void RowBoundTightener::setDimensions()
{
    freeMemoryIfNeeded();

    _n = _tableau.getN();
    _m = _tableau.getM();

    if ( GlobalConfiguration::EXPLICIT_BASIS_BOUND_TIGHTENING_TYPE ==
         GlobalConfiguration::COMPUTE_INVERTED_BASIS_MATRIX )
    {
        _rows = new TableauRow *[_m];
        for ( unsigned i = 0; i < _m; ++i )
            _rows[i] = new TableauRow( _n - _m );
    }
    else if ( GlobalConfiguration::EXPLICIT_BASIS_BOUND_TIGHTENING_TYPE ==
              GlobalConfiguration::USE_IMPLICIT_INVERTED_BASIS_MATRIX )
    {
        _rows = new TableauRow *[_m];
        for ( unsigned i = 0; i < _m; ++i )
            _rows[i] = new TableauRow( _n - _m );

        _z = new double[_m];
    }

    _ciTimesLb = new double[_n];
    _ciTimesUb = new double[_n];
    _ciSign = new char[_n];
}

RowBoundTightener::~RowBoundTightener()
{
    freeMemoryIfNeeded();
}

void RowBoundTightener::freeMemoryIfNeeded()
{
    if ( _rows )
    {
        for ( unsigned i = 0; i < _m; ++i )
            delete _rows[i];
        delete[] _rows;
        _rows = NULL;
    }

    if ( _z )
    {
        delete[] _z;
        _z = NULL;
    }

    if ( _ciTimesLb )
    {
        delete[] _ciTimesLb;
        _ciTimesLb = NULL;
    }

    if ( _ciTimesUb )
    {
        delete[] _ciTimesUb;
        _ciTimesUb = NULL;
    }

    if ( _ciSign )
    {
        delete[] _ciSign;
        _ciSign = NULL;
    }
}

void RowBoundTightener::examineImplicitInvertedBasisMatrix( bool untilSaturation )
{
    /*
      Roughly (the dimensions don't add up):

         xB = inv(B)*b - inv(B)*An
    */

    // Find z = inv(B) * b, by solving the forward transformation Bz = b
    _tableau.forwardTransformation( _tableau.getRightHandSide(), _z );
    for ( unsigned i = 0; i < _m; ++i )
    {
        _rows[i]->_scalar = _z[i];
        _rows[i]->_lhs = _tableau.basicIndexToVariable( i );
    }

    // Now, go over the columns of the constraint martrix, perform an FTRAN
    // for each of them, and populate the rows.
    for ( unsigned i = 0; i < _n - _m; ++i )
    {
        unsigned nonBasic = _tableau.nonBasicIndexToVariable( i );
        const double *ANColumn = _tableau.getAColumn( nonBasic );
        _tableau.forwardTransformation( ANColumn, _z );

        for ( unsigned j = 0; j < _m; ++j )
        {
            _rows[j]->_row[i]._var = nonBasic;
            _rows[j]->_row[i]._coefficient = -_z[j];
        }
    }

    // We now have all the rows, can use them for tightening.
    // The tightening procedure may throw an exception, in which case we need
    // to release the rows.
    unsigned newBoundsLearned;
    unsigned maxNumberOfIterations = untilSaturation ?
        GlobalConfiguration::ROW_BOUND_TIGHTENER_SATURATION_ITERATIONS : 1;
    do
    {
        newBoundsLearned = onePassOverInvertedBasisRows();

        if ( _statistics && ( newBoundsLearned > 0 ) )
            _statistics->incLongAttr( Statistics::NUM_EXPLICIT_BASIS_BOUND_TIGHTENING, newBoundsLearned );

        --maxNumberOfIterations;
    }
    while ( ( maxNumberOfIterations != 0 ) && ( newBoundsLearned > 0 ) );
}

void RowBoundTightener::examineInvertedBasisMatrix( bool untilSaturation )
{
    /*
      Roughly (the dimensions don't add up):

         xB = inv(B)*b - inv(B)*An

      We compute one row at a time.
    */

    const double *b = _tableau.getRightHandSide();
    const double *invB = _tableau.getInverseBasisMatrix();

    try
    {
        for ( unsigned i = 0; i < _m; ++i )
        {
            TableauRow *row = _rows[i];
            // First, compute the scalar, using inv(B)*b
            row->_scalar = 0;
            for ( unsigned j = 0; j < _m; ++j )
                row->_scalar += ( invB[i * _m + j] * b[j] );

            // Now update the row's coefficients for basic variable i
            for ( unsigned j = 0; j < _n - _m; ++j )
            {
                row->_row[j]._var = _tableau.nonBasicIndexToVariable( j );

                // Dot product of the i'th row of inv(B) with the appropriate
                // column of An

                const SparseUnsortedList *column = _tableau.getSparseAColumn( row->_row[j]._var );
                row->_row[j]._coefficient = 0;

                for ( const auto &entry : *column )
                    row->_row[j]._coefficient -= invB[i*_m + entry._index] * entry._value;
            }

            // Store the lhs variable
            row->_lhs = _tableau.basicIndexToVariable( i );
        }

        // We now have all the rows, can use them for tightening.
        // The tightening procedure may throw an exception, in which case we need
        // to release the rows.

        unsigned newBoundsLearned;
        unsigned maxNumberOfIterations = untilSaturation ?
            GlobalConfiguration::ROW_BOUND_TIGHTENER_SATURATION_ITERATIONS : 1;
        do
        {
            newBoundsLearned = onePassOverInvertedBasisRows();

            if ( _statistics && ( newBoundsLearned > 0 ) )
                _statistics->incLongAttr( Statistics::NUM_EXPLICIT_BASIS_BOUND_TIGHTENING, newBoundsLearned );

            --maxNumberOfIterations;
        }
        while ( ( maxNumberOfIterations != 0 ) && ( newBoundsLearned > 0 ) );
    }
    catch ( ... )
    {
        delete[] invB;
        throw;
    }

    delete[] invB;
}

unsigned RowBoundTightener::onePassOverInvertedBasisRows()
{
    unsigned newBounds = 0;

    for ( unsigned i = 0; i < _m; ++i )
        newBounds += tightenOnSingleInvertedBasisRow( *( _rows[i] ) );

    return newBounds;
}

unsigned RowBoundTightener::tightenOnSingleInvertedBasisRow( const TableauRow &row )
{
	/*
      A row is of the form

         y = sum ci xi + b

      We wish to tighten once for y, but also once for every x.
    */
    unsigned n = _tableau.getN();
    unsigned m = _tableau.getM();

    unsigned result = 0;

    // Compute ci * lb, ci * ub, flag signs for all entries
    enum {
        ZERO = 0,
        POSITIVE = 1,
        NEGATIVE = 2,
    };

    for ( unsigned i = 0; i < n - m; ++i )
    {
        double ci = row[i];

        if ( FloatUtils::isZero( ci ) )
        {
            _ciSign[i] = ZERO;
            _ciTimesLb[i] = 0;
            _ciTimesUb[i] = 0;
            continue;
        }

        _ciSign[i] = FloatUtils::isPositive( ci ) ? POSITIVE : NEGATIVE;

        unsigned xi = row._row[i]._var;

        ASSERT( FloatUtils::lte( lowerBound( xi ), _boundManager.getLowerBound( xi ) ) );
        ASSERT( FloatUtils::gte( upperBound( xi ), _boundManager.getUpperBound( xi ) ) );
        _ciTimesLb[i] = ci * lowerBound( xi );
        _ciTimesUb[i] = ci * upperBound( xi );
    }

    // Start with a pass for y
    unsigned y = row._lhs;
    double uBound = row._scalar;
    double lBound = row._scalar;

    unsigned xi;
    double ci;

    for ( unsigned i = 0; i < n - m; ++i )
    {
        if ( _ciSign[i] == POSITIVE )
        {
            lBound += _ciTimesLb[i];
            uBound += _ciTimesUb[i];
        }
        else
        {
            lBound += _ciTimesUb[i];
            uBound += _ciTimesLb[i];
        }
    }

    result += registerTighterLowerBound( y, lBound );
    result += registerTighterUpperBound( y, uBound );

    if ( FloatUtils::gt( lowerBound( y ), upperBound( y ) ) )
    {
        ASSERT( FloatUtils::gt( _boundManager.getLowerBound( y ), _boundManager.getUpperBound( y ) ) );
        throw InfeasibleQueryException();
    }
    // Next, do a pass for each of the rhs variables.
    // For this, we wish to logically transform the equation into:
    //
    //     xi = 1/ci * ( y - sum cj xj - b )
    //
    // And then compute the upper/lower bounds for xi.
    //
    // However, for efficiency, we compute the lower and upper
    // bounds of the expression:
    //
    //         y - sum ci xi - b
    //
    // Then, when we consider xi we adjust the computed lower and upper
    // boudns accordingly.

    double auxLb = lowerBound( y ) - row._scalar;
    double auxUb = upperBound( y ) - row._scalar;

    // Now add ALL xi's
    for ( unsigned i = 0; i < n - m; ++i )
    {
        if ( _ciSign[i] == NEGATIVE )
        {
            auxLb -= _ciTimesLb[i];
            auxUb -= _ciTimesUb[i];
        }
        else
        {
            auxLb -= _ciTimesUb[i];
            auxUb -= _ciTimesLb[i];
        }
    }

    // Now consider each individual xi
    for ( unsigned i = 0; i < n - m; ++i )
    {
        // If ci = 0, nothing to do.
        if ( _ciSign[i] == ZERO )
            continue;

        lBound = auxLb;
        uBound = auxUb;

        // Adjust the aux bounds to remove xi
        if ( _ciSign[i] == NEGATIVE )
        {
            lBound += _ciTimesLb[i];
            uBound += _ciTimesUb[i];
        }
        else
        {
            lBound += _ciTimesUb[i];
            uBound += _ciTimesLb[i];
        }

        // Now divide everything by ci, switching signs if needed.
        ci = row[i];
        lBound = lBound / ci;
        uBound = uBound / ci;

        if ( _ciSign[i] == NEGATIVE )
        {
            double temp = uBound;
            uBound = lBound;
            lBound = temp;
        }

        // If a tighter bound is found, store it
        xi = row._row[i]._var;
        result += registerTighterLowerBound( xi, lBound );
        result += registerTighterUpperBound( xi, uBound );

        if ( FloatUtils::gt( lowerBound( xi ), upperBound( xi ) ) )
        {
            ASSERT( FloatUtils::gt( _boundManager.getLowerBound( xi ), _boundManager.getUpperBound( xi ) ) );

            throw InfeasibleQueryException();
        }
    }

    return result;
}

void RowBoundTightener::examineConstraintMatrix( bool untilSaturation )
{
    unsigned newBoundsLearned;

    /*
      If working until saturation, do single passes over the matrix until no new bounds
      are learned. Otherwise, just do a single pass.
    */
    unsigned maxNumberOfIterations = untilSaturation ?
        GlobalConfiguration::ROW_BOUND_TIGHTENER_SATURATION_ITERATIONS : 1;
    do
    {
        newBoundsLearned = onePassOverConstraintMatrix();

        if ( _statistics && ( newBoundsLearned > 0 ) )
            _statistics->incLongAttr( Statistics::NUM_CONSTRAINT_MATRIX_BOUND_TIGHTENING,  newBoundsLearned );

        --maxNumberOfIterations;
    }
    while ( ( maxNumberOfIterations != 0 ) && ( newBoundsLearned > 0 ) );
}

unsigned RowBoundTightener::onePassOverConstraintMatrix()
{
    unsigned result = 0;

    unsigned m = _tableau.getM();

    for ( unsigned i = 0; i < m; ++i )
        result += tightenOnSingleConstraintRow( i );

    return result;
}

unsigned RowBoundTightener::tightenOnSingleConstraintRow( unsigned row )
{
    /*
      The cosntraint matrix A satisfies Ax = b.
      Each row is of the form:

          sum ci xi - b = 0

      We first compute the lower and upper bounds for the expression

          sum ci xi - b
          if ( FloatUtils::lt( lowerBound( xi ), lBound ) )
          {
          ASSERT( FloatUtils::lt( _boundManager.getLowerBound( xi ), lBound ) );

          _lowerBounds[xi] = lBound;
          _tightenedLower[xi] = true;
          ++result;
          }

          if ( FloatUtils::gt( upperBound( xi ), uBound ) )
          {
          ASSERT( FloatUtils::gt( _boundManager.getUpperBound( xi ), uBound ) );

          _upperBounds[xi] = uBound;
          _tightenedUpper[xi] = true;
          ++result;
          }

   */
    unsigned n = _tableau.getN();

    unsigned result = 0;

    const SparseUnsortedList *sparseRow = _tableau.getSparseARow( row );
    const double *b = _tableau.getRightHandSide();

    double ci;
    unsigned index;

    // Compute ci * lb, ci * ub, flag signs for all entries
    enum {
        ZERO = 0,
        POSITIVE = 1,
        NEGATIVE = 2,
    };

    std::fill_n( _ciSign, n, ZERO );
    std::fill_n( _ciTimesLb, n, 0 );
    std::fill_n( _ciTimesUb, n, 0 );

    for ( const auto &entry : *sparseRow )
    {
        index = entry._index;
        ci = entry._value;

        _ciSign[index] = FloatUtils::isPositive( ci ) ? POSITIVE : NEGATIVE;
        _ciTimesLb[index] = ci * lowerBound( index );
        _ciTimesUb[index] = ci * upperBound( index );
    }

    /*
      Do a pass for each of the rhs variables.
      For this, we wish to logically transform the equation into:

          xi = 1/ci * ( b - sum cj xj )

      And then compute the upper/lower bounds for xi.

      However, for efficiency, we compute the lower and upper
      bounds of the expression:

              b - sum ci xi

      Then, when we consider xi we adjust the computed lower and upper
      boudns accordingly.
    */

    double auxLb = b[row];
    double auxUb = b[row];

    // Now add ALL xi's
    for ( unsigned i = 0; i < n; ++i )
    {
        if ( _ciSign[i] == NEGATIVE )
        {
            auxLb -= _ciTimesLb[i];
            auxUb -= _ciTimesUb[i];
        }
        else
        {
            auxLb -= _ciTimesUb[i];
            auxUb -= _ciTimesLb[i];
        }
    }

    double lBound;
    double uBound;

    // Now consider each individual xi with non zero coefficient
    for ( const auto &entry : *sparseRow )
    {
        index = entry._index;

        lBound = auxLb;
        uBound = auxUb;

        // Adjust the aux bounds to remove xi
        if ( _ciSign[index] == NEGATIVE )
        {
            lBound += _ciTimesLb[index];
            uBound += _ciTimesUb[index];
        }
        else
        {
            lBound += _ciTimesUb[index];
            uBound += _ciTimesLb[index];
        }

        // Now divide everything by ci, switching signs if needed.
        ci = entry._value;

        lBound = lBound / ci;
        uBound = uBound / ci;

        if ( _ciSign[index] == NEGATIVE )
        {
            double temp = uBound;
            uBound = lBound;
            lBound = temp;
        }

        // If a tighter bound is found, store it
        result += registerTighterLowerBound( index, lBound );
        result += registerTighterUpperBound( index, uBound );

        if ( FloatUtils::gt( lowerBound( index ), upperBound( index ) ) )
        {
            ASSERT( FloatUtils::gt( _boundManager.getLowerBound( index ), _boundManager.getUpperBound( index ) ) );
            throw InfeasibleQueryException();
        }
    }

    return result;
}

void RowBoundTightener::setStatistics( Statistics *statistics )
{
    _statistics = statistics;
}

void RowBoundTightener::notifyDimensionChange( unsigned /* m */ , unsigned /* n */ )
{
    setDimensions();
}

//
// Local Variables:
// compile-command: "make -C ../.. "
// tags-file-name: "../../TAGS"
// c-basic-offset: 4
// End:
//