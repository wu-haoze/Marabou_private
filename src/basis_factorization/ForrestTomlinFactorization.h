/*********************                                                        */
/*! \file ForrestTomlinFactorization.h
 ** \verbatim
 ** Top contributors (to current version):
 **   Guy Katz
 ** This file is part of the Marabou project.
 ** Copyright (c) 2016-2017 by the authors listed in the file AUTHORS
 ** in the top-level source directory) and their institutional affiliations.
 ** All rights reserved. See the file COPYING in the top-level source
 ** directory for licensing information.\endverbatim
 **/

#ifndef __ForrestTomlinFactorization_h__
#define __ForrestTomlinFactorization_h__

#include "IBasisFactorization.h"

class ForrestTomlinFactorization : public IBasisFactorization
{
public:
    ForrestTomlinFactorization( unsigned m );
    ~ForrestTomlinFactorization();

    /*
      Inform the basis factorization that the basis has been changed
      by a pivot step. This results is an eta matrix by which the
      basis is multiplied on the right. This eta matrix is represented
      by the column index and column vector.
    */
    void pushEtaMatrix( unsigned columnIndex, double *column );

    /*
      Perform a forward transformation, i.e. find x such that Bx = y.
      Result needs to be of size m.
    */
    void forwardTransformation( const double *y, double *x ) const;

    /*
      Perform a forward transformation, i.e. find x such that xB = y.
      Result needs to be of size m.
    */
    void backwardTransformation( const double *y, double *x ) const;

    /*
      Store/restore the basis factorization.
    */
    void storeFactorization( IBasisFactorization *other );
    void restoreFactorization( const IBasisFactorization *other );

	/*
      Set the basis matrix.
	*/
	void setBasis( const double *B );

    /*
      Control/check whether factorization is enabled.
    */
    bool factorizationEnabled() const;
    void toggleFactorization( bool value );

    /*
      Return true iff the basis matrix B is explicitly available.
    */
    bool explicitBasisAvailable() const;

    /*
      Make the basis explicitly available
    */
    void makeExplicitBasisAvailable();

    /*
      Get the explicit basis matrix
    */
    const double *getBasis() const;

    /*
      Compute the inverse of B (should only be called when B is explicitly available).
     */
    void invertBasis( double *result );
};

#endif // __ForrestTomlinFactorization_h__

//
// Local Variables:
// compile-command: "make -C ../.. "
// tags-file-name: "../../TAGS"
// c-basic-offset: 4
// End:
//