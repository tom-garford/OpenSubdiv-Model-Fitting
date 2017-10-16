// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2017 Jan Svoboda <jan.svoboda@usi.ch>
// Copyright (C) 2016 Andrew Fitzgibbon <awf@microsoft.com>
// Copyright (C) 2009 Thomas Capricelli <orzel@freehackers.org>
//
// This code initially comes from MINPACK whose original authors are:
// Copyright Jorge More - Argonne National Laboratory
// Copyright Burt Garbow - Argonne National Laboratory
// Copyright Ken Hillstrom - Argonne National Laboratory
//
// This Source Code Form is subject to the terms of the Minpack license
// (a BSD-like license) described in the campaigned CopyrightMINPACK.txt file.

#ifndef EIGEN_LMONESTEP_H
#define EIGEN_LMONESTEP_H

#include <iomanip>

namespace Eigen {

	template<typename FunctorType>
	LevenbergMarquardtSpace::Status
		LevenbergMarquardt<FunctorType>::minimizeOneStep(InputType &x)
	{

		using std::abs;
		using std::sqrt;

		RealScalar pnorm, fnorm1, actred, dirder, prered;

		RealScalar temp = 0.0;
		RealScalar xnorm = 0.0;
		/* calculate the jacobian matrix. */
		m_fjac.setZero();
		Index df_ret = m_functor.df(x, m_fjac);

		eigen_assert(m_fjac.cols() == n);
		if (df_ret < 0) {
			return LevenbergMarquardtSpace::UserAsked;
		}
		if (df_ret > 0) {
			// numerical diff, we evaluated the function df_ret times
			m_nfev += df_ret;
		} else {
			m_njev++;
		}

		if (m_verbose) std::cout << std::setw(4) << m_iter << ": ";

		// Compute Jacobian column norms 
		for (int j = 0; j < m_fjac.cols(); ++j) {
			m_col_norms(j) = m_fjac.col(j).blueNorm();
		}

		/* compute the qr factorization of the jacobian. */
		m_qrfac.compute(m_fjac);

		if (m_qrfac.info() != Success) {
			m_info = NumericalIssue;
			if (m_verbose) std::cout << "QR Failed\n";
			return LevenbergMarquardtSpace::ImproperInputParameters;
		}
		// Make a copy of the first factor with the associated permutation
		m_rfactor = m_qrfac.matrixR();
		m_permutation = (m_qrfac.colsPermutation());

		/* on the first iteration and if external scaling is not used, scale according */
		/* to the norms of the columns of the initial jacobian. */
		if (m_iter == 1) {
			if (!m_useExternalScaling) {
				for (Index j = 0; j < n; ++j) {
					m_diag[j] = (m_col_norms[j] == 0.) ? 1. : m_col_norms[j];
				}
			}

			/* on the first iteration, calculate the norm of the scaled x */
			/* and initialize the step bound m_delta. */
			xnorm = m_functor.estimateNorm(x, m_diag);
			m_delta = m_factor * xnorm;
			if (m_delta == 0.) {
				m_delta = m_factor;
			}
		}

		/* form (q transpose)*m_fvec and store the first n components in */
		/* m_qtf. */
		{
			m_qtfFull = (m_qrfac.matrixQ().adjoint() * m_fvec);
			m_qtf = m_qtfFull.head(n);
		}

		/* compute the norm of the scaled gradient. */
		m_gnorm = 0.;
		if (m_fnorm != 0.) {
			for (Index j = 0; j < n; ++j) {
				if (m_col_norms[m_permutation.indices()[j]] != 0.) {
					m_gnorm = (std::max)(m_gnorm, abs(m_rfactor.col(j).head(j + 1).dot(m_qtf.head(j + 1) / m_fnorm) / m_col_norms[m_permutation.indices()[j]]));
				}
			}
		}

		/* test for convergence of the gradient norm. */
		if (m_gnorm <= m_gtol) {
			m_info = Success;
			if (m_verbose) std::cout << "Done, gnorm = " << m_gnorm << "\n";
			return LevenbergMarquardtSpace::CosinusTooSmall;
		}

		/* rescale if necessary. */
		if (!m_useExternalScaling) {
			m_diag = m_diag.cwiseMax(m_col_norms);
		}

		bool successful_iter = false;
		do {
			/* determine the levenberg-marquardt parameter. */
			StepType step(n);
			internal::lmpar2<QRSolver, StepType>(m_qrfac, m_diag, m_qtf, m_delta, m_par, step);

			/* store the direction p and x + p. calculate the norm of p. */
			step = -step;
			m_xtmp = x; // Copy current x
			m_functor.increment_in_place(&m_xtmp, step);
			pnorm = m_diag.cwiseProduct(step).stableNorm();

			/* on the first iteration, adjust the initial step bound. */
			if (m_iter == 1) {
				m_delta = (std::min)(m_delta, pnorm);
			}

			if (m_verbose) std::cout << "D" << m_delta << ", ";

			/* evaluate the function at x + p and calculate its norm. */
			ValueType fvec(m);
			if (m_functor(m_xtmp, fvec) < 0) {
				if (m_verbose) std::cout << "Functor signalled to stop\n";
				return LevenbergMarquardtSpace::UserAsked;
			}
			++m_nfev;
			fnorm1 = fvec.stableNorm();

			if (m_verbose) std::cout << "F=" << fnorm1 << ", ";

			/* compute the scaled actual reduction. */
			actred = -1.;
			if (Scalar(.1) * fnorm1 < m_fnorm) {
				actred = 1. - numext::abs2(fnorm1 / m_fnorm);
			}

			/* compute the scaled predicted reduction and */
			/* the scaled directional derivative. */
			m_rStep = m_rfactor.template triangularView<Upper>() * (m_permutation.inverse() *step);
			RealScalar temp1 = numext::abs2(m_rStep.stableNorm() / m_fnorm);
			RealScalar temp2 = numext::abs2(sqrt(m_par) * pnorm / m_fnorm);
			prered = temp1 + temp2 / Scalar(.5);
			dirder = -(temp1 + temp2);

			/* compute the ratio of the actual to the predicted */
			/* reduction. */
			RealScalar ratio = 0.;
			if (prered != 0.) {
				ratio = actred / prered;
			}

			if (m_verbose) std::cout << "Rat=" << ratio << ", ";
			/* update the step bound. */
			if (ratio <= Scalar(.25)) {
				if (actred >= 0.) {
					temp = RealScalar(.5);
				}
				if (actred < 0.) {
					temp = RealScalar(.5) * dirder / (dirder + RealScalar(.5) * actred);
				}
				if (RealScalar(.1) * fnorm1 >= m_fnorm || temp < RealScalar(.1)) {
					temp = Scalar(.1);
				}
				/* Computing MIN */
				m_delta = temp * (std::min)(m_delta, pnorm / RealScalar(.1));
				m_par /= temp;
			}
			else if (!(m_par != 0. && ratio < RealScalar(.75))) {
				m_delta = pnorm / RealScalar(.5);
				m_par = RealScalar(.5) * m_par;
			}

			/* test for successful iteration. */
			if (ratio >= RealScalar(1e-4)) {
				/* successful iteration. update x, m_fvec, and their norms. */
				x = m_xtmp;
				m_fvec = fvec;
				xnorm = m_functor.estimateNorm(x, m_diag);
				m_fnorm = fnorm1;
				++m_iter;
				successful_iter = true;
			}

			// if (m_verbose) std::cout << "[" << toc() << " sec] ";

			/* tests for convergence. */
			if (abs(actred) <= m_ftol && prered <= m_ftol && Scalar(.5) * ratio <= 1. && m_delta <= m_xtol * xnorm) {
				m_info = Success;
				if (m_verbose) std::cout << "RelativeErrorAndReductionTooSmall\n";
				return LevenbergMarquardtSpace::RelativeErrorAndReductionTooSmall;
			}
			if (abs(actred) <= m_ftol && prered <= m_ftol && Scalar(.5) * ratio <= 1.) {
				m_info = Success;
				if (m_verbose) std::cout << "RelativeReductionTooSmall\n";
				return LevenbergMarquardtSpace::RelativeReductionTooSmall;
			}
			if (m_delta <= m_xtol * xnorm) {
				m_info = Success;
				if (m_verbose) std::cout << "RelativeErrorTooSmall\n";
				return LevenbergMarquardtSpace::RelativeErrorTooSmall;
			}

			/* tests for termination and stringent tolerances. */
			if (m_nfev >= m_maxfev) {
				m_info = NoConvergence;
				if (m_verbose) std::cout << "TooManyFunctionEvaluation\n";
				return LevenbergMarquardtSpace::TooManyFunctionEvaluation;
			}
			if (abs(actred) <= NumTraits<Scalar>::epsilon() && prered <= NumTraits<Scalar>::epsilon() && Scalar(.5) * ratio <= 1.) {
				m_info = Success;
				if (m_verbose) std::cout << "FtolTooSmall\n";
				return LevenbergMarquardtSpace::FtolTooSmall;
			}
			if (m_delta <= NumTraits<Scalar>::epsilon() * xnorm) {
				m_info = Success;
				if (m_verbose) std::cout << "XtolTooSmall\n";
				return LevenbergMarquardtSpace::XtolTooSmall;
			}
			if (m_gnorm <= NumTraits<Scalar>::epsilon()) {
				m_info = Success;
				if (m_verbose) std::cout << "GtolTooSmall\n";
				return LevenbergMarquardtSpace::GtolTooSmall;
			}

		} while (!successful_iter);

		if (m_verbose) std::cout << "\n";
		return LevenbergMarquardtSpace::Running;
	}


} // end namespace Eigen

#endif // EIGEN_LMONESTEP_H
