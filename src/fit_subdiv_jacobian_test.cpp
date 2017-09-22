#define _wassert wassert_awf
#include <cassert>

#include <iostream>
#include <iomanip>

#include <random>

#include <Eigen/Eigen>

#include "log3d.h"

#include "FPJParser.h"
#include "PLYParser.h"
#include "Logger.h"
#include "BezierPatch.h"
#include "RigidTransform.h"

#include "Optimization/PosOnlyFunctor.h"
#include "Optimization/PosOnlyWithRegFunctor.h"
#include "Optimization/PosAndNormalsFunctor.h"
#include "Optimization/PosAndNormalsWithRegFunctor.h"

//typedef PosOnlyFunctor OptimizationFunctor;
typedef PosOnlyWithRegFunctor OptimizationFunctor;
//typedef PosAndNormalsFunctor OptimizationFunctor;
//typedef PosAndNormalsWithRegFunctor OptimizationFunctor;

using namespace Eigen;


void logmesh(log3d& log, MeshTopology const& mesh, Matrix3X const& vertices) {
	Matrix3Xi tris(3, mesh.quads.cols() * 2);
	
	tris.block(0, 0, 1, mesh.quads.cols()) = mesh.quads.row(0);
	tris.block(1, 0, 1, mesh.quads.cols()) = mesh.quads.row(2);
	tris.block(2, 0, 1, mesh.quads.cols()) = mesh.quads.row(1);
	tris.block(0, mesh.quads.cols(), 1, mesh.quads.cols()) = mesh.quads.row(0);
	tris.block(1, mesh.quads.cols(), 1, mesh.quads.cols()) = mesh.quads.row(3);
	tris.block(2, mesh.quads.cols(), 1, mesh.quads.cols()) = mesh.quads.row(2);
	/*
	tris.block(0, 0, 1, mesh.quads.cols()) = mesh.quads.row(3);
	tris.block(1, 0, 1, mesh.quads.cols()) = mesh.quads.row(1);
	tris.block(2, 0, 1, mesh.quads.cols()) = mesh.quads.row(2);
	tris.block(0, mesh.quads.cols(), 1, mesh.quads.cols()) = mesh.quads.row(3);
	tris.block(1, mesh.quads.cols(), 1, mesh.quads.cols()) = mesh.quads.row(0);
	tris.block(2, mesh.quads.cols(), 1, mesh.quads.cols()) = mesh.quads.row(1);
	*/
	log.mesh(tris, vertices);
	
}

void logsubdivmesh(log3d& log, MeshTopology const& mesh, Matrix3X const& vertices) {
	log.wiremesh(mesh.quads, vertices);
	SubdivEvaluator evaluator(mesh);
	MeshTopology refined_mesh;
	Matrix3X refined_verts;
	evaluator.generate_refined_mesh(vertices, 3, &refined_mesh, &refined_verts);
	logmesh(log, refined_mesh, refined_verts);
}

// Initialize UVs to the middle of each face
void initializeUVs(MeshTopology &mesh, OptimizationFunctor::InputType &params, const Matrix3X &data) {
	int nFaces = int(mesh.quads.cols());
	int nDataPoints = int(data.cols());

	// 1. Make a list of test points, e.g. centre point of each face
	Matrix3X test_points(3, nFaces);
	std::vector<SurfacePoint> uvs{ size_t(nFaces),{ 0,{ 0.5, 0.5 } } };
	for (int i = 0; i < nFaces; ++i)
		uvs[i].face = i;

	SubdivEvaluator evaluator(mesh);
	evaluator.evaluateSubdivSurface(params.control_vertices, uvs, &test_points);
	
	for (int i = 0; i < nDataPoints; i++) {
		// Closest test point
		Eigen::Index test_pt_index;
		(test_points.colwise() - data.col(i)).colwise().squaredNorm().minCoeff(&test_pt_index);
		params.us[i] = uvs[test_pt_index];
	}
}

// Transformation of 3D model to the initial alignment
void transform3D(const Matrix3X &points3D, Matrix3X &points3DOut, const FPJParser::ImageFile &imageParams) {
	int nDataPts = int(points3D.cols());

	// Convert points into homogeneous coordinates
	MatrixXd pts3DTransf = MatrixXd::Ones(nDataPts, 4);
	pts3DTransf.block(0, 0, nDataPts, 3) << points3D.transpose();
	// Apply positioning transformation (translation, rotation, scale)
	pts3DTransf = pts3DTransf * imageParams.rigidTransf.transformation().cast<Scalar>();

	// Ortographic projection into 2D plane defined as Phi([x, y, z]) = [x, y]
	points3DOut << pts3DTransf.block(0, 0, nDataPts, 3).transpose();
}

// Projection of 3D model into 2D
void project3DTo2D(const Matrix3X &points3D, Matrix2X &points2D, const FPJParser::ImageFile &imageParams) {
	int nDataPts = int(points3D.cols());
	Matrix3X points3DTransf(3, nDataPts);
	transform3D(points3D, points3DTransf, imageParams);

	// Ortographic projection into 2D plane defined as Phi([x, y, z]) = [x, y]
	points2D << points3DTransf.block(0, 0, 2, nDataPts);
}

int main() {
	Logger::createLogger("runtime_log.log");
	Logger::instance()->log(Logger::Info, "Computation STARTED!");

	// Load banana model
	PLYParser plyParse("Z:/OpenSubdiv-Model-Fitting/build/Debug/banana_quad_coarse.ply");
	plyParse.parse(PLYParser::Model::Quads);
	// Get the user input image parameters
	FPJParser fpjParse("Z:/OpenSubdiv-Model-Fitting/build/Debug/projects/bananas.fpj");
	fpjParse.parse();
	
	const unsigned int nParamVals = 10;
	float t[nParamVals] = { 0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9 };
	int nDataPoints = int(fpjParse.project().images[0].silhouettePoints[0].rows()) * nParamVals;
	std::stringstream ss;
	ss << "Number of data points: " << nDataPoints;
	Logger::instance()->log(Logger::Info, ss.str());
	Matrix3X data(3, nDataPoints);
	Matrix3X dataNormals(3, nDataPoints);
	std::default_random_engine gen;
	std::uniform_real_distribution<double> dist(0.0, 1.0);
	for (int i = 0; i < int(fpjParse.project().images[0].silhouettePoints[0].rows()); i++) {
		for (int j = 0; j < nParamVals; j++) {
			Eigen::Vector2f pt = BezierPatch::evaluateAt(fpjParse.project().images[0].silhouettePoints[0].row(i),
				fpjParse.project().images[0].silhouettePoints[1].row(i),
				fpjParse.project().images[0].silhouettePoints[2].row(i),
				fpjParse.project().images[0].silhouettePoints[3].row(i), t[j]);
			data(0, nParamVals * i + j) = pt(0);
			data(1, nParamVals * i + j) = pt(1);
			data(2, nParamVals * i + j) = 0.0f;// dist(gen) * 0.125 - 0.125 / 2.0;
			
			Eigen::Vector2f n = BezierPatch::evaluateNormalAt(fpjParse.project().images[0].silhouettePoints[0].row(i),
				fpjParse.project().images[0].silhouettePoints[1].row(i),
				fpjParse.project().images[0].silhouettePoints[2].row(i),
				fpjParse.project().images[0].silhouettePoints[3].row(i), t[j], false);
			n.normalize();
			dataNormals(0, nParamVals * i + j) = n(0);
			dataNormals(1, nParamVals * i + j) = n(1);
			dataNormals(2, nParamVals * i + j) = 0.0f;
		}
	}

	// Draw one of the silhouettes
	for (int i = 0; i < int(fpjParse.project().images[0].silhouettePoints[0].rows()); i++) {
		Eigen::Vector2f pt = BezierPatch::evaluateAt(fpjParse.project().images[0].silhouettePoints[0].row(i),
			fpjParse.project().images[0].silhouettePoints[1].row(i), 
			fpjParse.project().images[0].silhouettePoints[2].row(i), 
			fpjParse.project().images[0].silhouettePoints[3].row(i), 0.5);
	}
	
	// Make "control" cube
	MeshTopology mesh;
	Matrix3X control_vertices_gt;
	makeFromPLYModel(&mesh, &control_vertices_gt, plyParse.model());
	//makeCube(&mesh, &control_vertices_gt);	

	// INITIAL PARAMS
	OptimizationFunctor::InputType params;
	params.control_vertices = control_vertices_gt;// +0.1 * MatrixXX::Random(3, control_vertices_gt.cols());
	params.us.resize(nDataPoints);
	//params.rigidTransf.setTranslation(fpjParse.project().images[0].rigidTransf.params().t1, 
	//	fpjParse.project().images[0].rigidTransf.params().t2, 
	//	fpjParse.project().images[0].rigidTransf.params().t3);
	//Eigen::Vector3f barycenter = plyParse.model().barycenter();
	Eigen::Vector3f barycenter = MeshTopology::computeBarycenter(data);
	params.rigidTransf.setTranslation(barycenter(0), barycenter(1), barycenter(2));
	//params.rigidTransf.setRotation(fpjParse.project().images[0].rigidTransf.params().r1,
	//	fpjParse.project().images[0].rigidTransf.params().r2,
	//params.rigidTransf.setScaling(0.5, 0.5, 0.5);
	//params.rigidTransf.setScaling(1.0, 10.0, 5.0);
	params.rigidTransf.setScaling(fpjParse.project().images[0].rigidTransf.params().s1 * 2.0,
		fpjParse.project().images[0].rigidTransf.params().s2 * 2.0,
		fpjParse.project().images[0].rigidTransf.params().s3 * 2.0);
	
	// Initialize uvs.
	initializeUVs(mesh, params, data);
	
	OptimizationFunctor::DataConstraints constraints;
	//constraints.push_back(OptimizationFunctor::DataConstraint(0, 1));
	OptimizationFunctor functor(data, mesh, constraints);
	//OptimizationFunctor functor(data, dataNormals, mesh, constraints);
	

	// Check Jacobian
	std::cout << "Testing Jacobian ..." << std::endl;
	for (float eps = 1e-8f; eps < 1.1e-3f; eps *= 10.f) {
		NumericalDiff<OptimizationFunctor> fd{ functor, OptimizationFunctor::Scalar(eps) };
		OptimizationFunctor::JacobianType J;
		OptimizationFunctor::JacobianType J_fd;
		functor.df(params, J);
		fd.df(params, J_fd);
		double diff = (J - J_fd).norm();
		unsigned int num_uv = nDataPoints * 2;
		unsigned int num_tsr = 9;
		unsigned int num_xyz = mesh.num_vertices * 3;
		unsigned int num_res_p = nDataPoints * 3;
		unsigned int num_res_n = nDataPoints;// nDataPoints * 3;
		unsigned int num_res_c = constraints.size() * 3;
		unsigned int num_res_tp = num_xyz;
		if (diff > 0) {
			std::cout << "pn-xyz: " << (J.toDense().block(0, num_uv + num_tsr, num_res_p + num_res_n, num_xyz) - J_fd.toDense().block(0, num_uv + num_tsr, num_res_p + num_res_n, num_xyz)).norm() << std::endl;
			std::cout << "pn-uv: " << (J.toDense().block(0, 0, num_res_p + num_res_n, num_uv) - J_fd.toDense().block(0, 0, num_res_p + num_res_n, num_uv)).norm() << std::endl;
			std::cout << "pn-t: " << (J.toDense().block(0, num_uv, num_res_p + num_res_n, 3) - J_fd.toDense().block(0, num_uv, num_res_p + num_res_n, 3)).norm() << std::endl;
			std::cout << "pn-s: " << (J.toDense().block(0, num_uv + 3, num_res_p + num_res_n, 3) - J_fd.toDense().block(0, num_uv + 3, num_res_p + num_res_n, 3)).norm() << std::endl;
			std::cout << "pn-r: " << (J.toDense().block(0, num_uv + 6, num_res_p + num_res_n, 3) - J_fd.toDense().block(0, num_uv + 6, num_res_p + num_res_n, 3)).norm() << std::endl;
			std::cout << "pn-tsr: " << (J.toDense().block(0, num_uv, num_res_p + num_res_n, num_tsr) - J_fd.toDense().block(0, num_uv, num_res_p + num_res_n, num_tsr)).norm() << std::endl;
			std::cout << "c-xyz: " << (J.toDense().block(num_res_p + num_res_n, num_uv + num_tsr, num_res_c, num_xyz) - J_fd.toDense().block(num_res_p + num_res_n, num_uv + num_tsr, num_res_c, num_xyz)).norm() << std::endl;
			std::cout << "c-uv: " << (J.toDense().block(num_res_p + num_res_n, 0, num_res_c, num_uv) - J_fd.toDense().block(num_res_p + num_res_n, 0, num_res_c, num_uv)).norm() << std::endl;
			std::cout << "c-tsr: " << (J.toDense().block(num_res_p + num_res_n, num_uv, num_res_c, num_tsr) - J_fd.toDense().block(num_res_p + num_res_n, num_uv, num_res_c, num_tsr)).norm() << std::endl;
			std::cout << "tp-xyz: " << (J.toDense().block(num_res_p + num_res_n + num_res_c, num_uv + num_tsr, num_res_tp, num_xyz) - J_fd.toDense().block(num_res_p + num_res_n + num_res_c, num_uv + num_tsr, num_res_tp, num_xyz)).norm() << std::endl;
			std::cout << "tp-uv: " << (J.toDense().block(num_res_p + num_res_n + num_res_c, 0, num_res_tp, num_uv) - J_fd.toDense().block(num_res_p + num_res_n + num_res_c, 0, num_res_tp, num_uv)).norm() << std::endl;
			std::cout << "tp-tsr: " << (J.toDense().block(num_res_p + num_res_n + num_res_c, num_uv, num_res_tp, num_tsr) - J_fd.toDense().block(num_res_p + num_res_n + num_res_c, num_uv, num_res_tp, num_tsr)).norm() << std::endl;
				
			/*
			std::ofstream ofs("j_xyz_fd.csv");
			ofs << J_fd.toDense().block(0, num_uv + num_tsr, num_res_p + num_res_c + num_res_tp, num_xyz) << std::endl;
			ofs.close();

			std::ofstream ofs2("j_xyz_my.csv");
			ofs2 << J.toDense().block(0, num_uv + num_tsr, num_res_p + num_res_c + num_res_tp, num_xyz) << std::endl;
			ofs2.close();
			*/

			Logger::instance()->logMatrixCSV(J_fd.toDense().block(0, 0, num_res_p + num_res_n, num_uv), "j_pnuv_fd.csv");
			Logger::instance()->logMatrixCSV(J.toDense().block(0, 0, num_res_p + num_res_n, num_uv), "j_pnuv_my.csv");
			//Logger::instance()->logMatrixCSV(J_fd.toDense().block(num_res_p + num_res_c, num_uv, num_res_tp, num_tsr + num_xyz), "j_tp_fd.csv");
			//Logger::instance()->logMatrixCSV(J.toDense().block(num_res_p + num_res_c, num_uv, num_res_tp, num_tsr + num_xyz), "j_tp_my.csv");
				
			/*
			std::ofstream ofs("p_st_fd.csv");
			ofs << J_fd.toDense().block<560, 9>(0, 374) << std::endl;
			ofs.close();

			std::ofstream ofs2("p_st_my.csv");
			ofs2 << J.toDense().block<560, 9>(0, 374)<< std::endl;
			ofs2.close();
			*/
			/*std::ofstream ofs("tp-xyz_fd.csv");
			ofs << J_fd.toDense().block<24, 24>(153, 102) << std::endl;
			ofs.close();

			std::ofstream ofs2("tp-xyz_my.csv");
			ofs2 << J.toDense().block<24, 24>(153, 102) << std::endl;
			ofs2.close();*/

			std::stringstream ss;
			ss << "Jacobian diff(eps=" << eps << "), = " << diff;
			Logger::instance()->log(Logger::Debug, ss.str());
			Logger::instance()->logSparseMatrix(J, "J.txt");
			Logger::instance()->logSparseMatrix(J_fd, "J_fd.txt");
		}

		if (diff > 10.0) {
			std::cout << "Test Jacobian - ERROR TOO BIG, exitting..." << std::endl;
			return 0;
		}
	}
	std::cout << "Test Jacobian - DONE, exitting..." << std::endl;

	Logger::instance()->log(Logger::Info, "Computation DONE!");

	return 0;
}

// Override system assert so one can set a breakpoint in it rather than clicking "Retry" and "Break"
void __cdecl _wassert(_In_z_ wchar_t const* _Message, _In_z_ wchar_t const* _File, _In_ unsigned _Line)
{
	std::wcerr << _File << "(" << _Line << "): ASSERT FAILED [" << _Message << "]\n";

	abort();
}