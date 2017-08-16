#ifndef POSANDNORMALS_FUNCTOR_H
#define POSANDNORMALS_FUNCTOR_H

#include "BaseFunctor.h"

struct PosAndNormalsFunctor : public BaseFunctor {
	// Input normals
	Matrix3X data_normals;

	PosAndNormalsFunctor(const Matrix3X& data_points, const Matrix3X &data_normals, const MeshTopology& mesh);

	// Functor functions
	// 1. Evaluate the residuals at x
	virtual int operator()(const InputType& x, ValueType& fvec);

	// 2. Evaluate jacobian at x
	virtual int df(const InputType& x, JacobianType& fjac);

	// Update function
	virtual void increment_in_place(InputType* x, StepType const& p);

	virtual void initQRSolver(SchurlikeQRSolver &qr);
};

#endif
