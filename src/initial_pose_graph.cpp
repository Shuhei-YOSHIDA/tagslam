/* -*-c++-*--------------------------------------------------------------------
 * 2018 Bernd Pfrommer bernd.pfrommer@gmail.com
 */

#include "tagslam/initial_pose_graph.h"
#include "tagslam/utils.h"
#include "tagslam/resectioning_factor.h"
#include <boost/range/irange.hpp>
#include <gtsam/slam/expressions.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>
#include <gtsam/nonlinear/ExpressionFactorGraph.h>
#include <boost/random/normal_distribution.hpp>
#include <boost/random/variate_generator.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <gtsam/geometry/Cal3DS2.h>

  class Cal3DS2U : public gtsam::Cal3DS2 {
  public:
    Cal3DS2U(double fx, double fy, double s, double u0, double v0,
            double k1, double k2, double p1 = 0.0, double p2 = 0.0) :
      gtsam::Cal3DS2(fx, fy, s, u0, v0, k1, k2, p1, p2) {}
    Cal3DS2U(const gtsam::Cal3DS2 &cal) : Cal3DS2(cal) {
    }

    gtsam::Point2 uncalibrate(const gtsam::Point2& p,
                              gtsam::OptionalJacobian<2,9> Dcal = boost::none,
                              gtsam::OptionalJacobian<2,2> Dp = boost::none) const {
      return (gtsam::Cal3DS2_Base::uncalibrate(p, Dcal, Dp));
    }
  };

namespace gtsam {
template<>
struct traits<Cal3DS2U> : public internal::Manifold<Cal3DS2> {};
template<>
struct traits<const Cal3DS2U> : public internal::Manifold<Cal3DS2> {};
}

namespace tagslam {
  using namespace boost::random;
  using boost::irange;
  typedef boost::random::mt19937 RandEng;
  typedef boost::random::normal_distribution<double> RandDist;
  typedef boost::random::variate_generator<RandEng, RandDist> RandGen;

  static void
  from_gtsam(cv::Mat *rvec, cv::Mat *tvec, const gtsam::Pose3 &p) {
    const gtsam::Point3 rv = gtsam::Rot3::Logmap(p.rotation());
    const gtsam::Point3 tv = p.translation();
    *rvec = (cv::Mat_<double>(3,1) << rv.x(), rv.y(), rv.z());
    *tvec = (cv::Mat_<double>(3,1) << tv.x(), tv.y(), tv.z());
  }

  static void to_opencv(std::vector<cv::Point3d> *a,
                       const std::vector<gtsam::Point3> b) {
    for (const auto &p: b) {
      a->emplace_back(p.x(), p.y(), p.z());
    }
  }
  static void to_opencv(std::vector<cv::Point2d> *a,
                       const std::vector<gtsam::Point2> b) {
    for (const auto &p: b) {
      a->emplace_back(p.x(), p.y());
    }
  }

  static gtsam::Pose3 make_random_pose(RandGen *rgr, RandGen *rgt) {
    gtsam::Point3 t((*rgt)(), (*rgt)(), (*rgt)());
    gtsam::Point3 om((*rgr)(), (*rgr)(), (*rgr)());
    return (gtsam::Pose3(gtsam::Rot3::rodriguez(om.x(),om.y(),om.z()), gtsam::Point3(t)));
  }

  static void print_pose(const gtsam::Pose3 &p) {
    const auto mat = p.matrix();
    std::cout << "[" << mat(0,0) << ", " << mat(0,1) << ", " << mat(0,2) << ", " << mat(0,3) << "; " << std::endl;
    std::cout << " " << mat(1,0) << ", " << mat(1,1) << ", " << mat(1,2) << ", " << mat(1,3) << "; " << std::endl;
    std::cout << " " << mat(2,0) << ", " << mat(2,1) << ", " << mat(2,2) << ", " << mat(2,3) << "; " << std::endl;
    std::cout << " " << mat(3,0) << ", " << mat(3,1) << ", " << mat(3,2) << ", " << mat(3,3) << "; " << std::endl;
    std::cout << "];" << std::endl;
  }
  static PoseEstimate try_optimization(const gtsam::Pose3 &startPose,
                                       const gtsam::Values &startValues,
                                       gtsam::NonlinearFactorGraph *graph) {
    gtsam::Symbol P = gtsam::Symbol('P', 0); // pose symbol
    gtsam::Values                 values = startValues;
    gtsam::Values                 optimizedValues;
    values.insert(P, startPose);
    gtsam::LevenbergMarquardtParams lmp;
    lmp.setVerbosity("SILENT");
    const int MAX_ITER = 100;
    lmp.setMaxIterations(MAX_ITER);
    lmp.setAbsoluteErrorTol(1e-7);
    lmp.setRelativeErrorTol(0);
    try {
      gtsam::LevenbergMarquardtOptimizer lmo(*graph, values, lmp);
      optimizedValues = lmo.optimize();
      gtsam::Pose3 op = optimizedValues.at<gtsam::Pose3>(P);
      return (PoseEstimate(op, (double)lmo.error() / graph->size(),
                           (int)lmo.iterations()));
    } catch (const std::exception &e) {
      // bombed out because of cheirality etc
    }
    return (PoseEstimate(startPose, 1e10, MAX_ITER));
  }

  static void analyze_pose(const CameraVec &cams,
                           const RigidBodyConstPtr &rb,
                           const gtsam::Pose3 &bodyPose) {
    for (const auto &tagMap: rb->observedTags) {
      int cam_idx = tagMap.first;
      std::cout << "points + projected for cam " << cam_idx << std::endl;
      const CameraPtr &cam = cams[cam_idx];
      std::cout << "cam pose: " << std::endl;
      print_pose(cam->poseEstimate.getPose());
      Cal3DS2U cK(*cam->gtsamCameraModel);
      if (!cam->poseEstimate.isValid()) {
        continue;
      }
      gtsam::PinholeCamera<gtsam::Cal3DS2> phc(cam->poseEstimate.getPose(), *cam->gtsamCameraModel);
      cv::Mat rvec, tvec;
      //from_gtsam(&rvec, &tvec, cam->poseEstimate.getPose().inverse());
      from_gtsam(&rvec, &tvec, cam->poseEstimate.getPose().inverse());
      std::vector<gtsam::Point3> bpts;
      std::vector<gtsam::Point3> wpts;
      std::vector<gtsam::Point2> ipts;
      rb->getAttachedPoints(cam_idx, &bpts, &ipts);
      for (const auto i: irange(0ul, bpts.size())) {
        wpts.push_back(bodyPose.transform_from(bpts[i]));
      }
      std::vector<cv::Point3d> wp;
      std::vector<cv::Point2d> ipp;
      to_opencv(&wp, wpts);
      const auto &ci = cam->intrinsics;
      utils::project_points(wp, rvec, tvec, ci.K,
                            ci.distortion_model, ci.D, &ipp);
      std::cout << "ppts=[ ";
      for (const auto i: irange(0ul, wp.size())) {
        gtsam::Point3 wp  = wpts[i];
        gtsam::Point2 icp = phc.project(wp);
        std::cout << wpts[i].x() << "," << wpts[i].y() << "," << wpts[i].z() << ", " << ipts[i].x() << ", " << ipts[i].y() << ", " << icp.x() << ", " <<icp.y() << ";" <<  std::endl;
      }
      std::cout << "];" << std::endl;
    }
  }

  PoseEstimate
  InitialPoseGraph::estimateBodyPose(const CameraVec &cams,
                                     const RigidBodyConstPtr &rb,
                                     const gtsam::Pose3 &initialPose) const {
    std::cout << "----------------- analysis of initial pose -----" << std::endl;
    analyze_pose(cams, rb, initialPose);
    PoseEstimate pe; // defaults to invalid
    gtsam::ExpressionFactorGraph  graph;
    std::cout << "estimating body pose from cameras: " << rb->observedTags.size() << std::endl;
    std::cout << "initial guess pose: " << std::endl;
    print_pose(initialPose);
    // loop through all tags on body
    auto pixelNoise = gtsam::noiseModel::Isotropic::Sigma(2, 1.0);
    gtsam::Pose3_  T_w_b('P', 0);
    for (const auto &tagMap: rb->observedTags) {
      int cam_idx = tagMap.first;
      const CameraPtr &cam = cams[cam_idx];
      if (!cam->poseEstimate.isValid()) {
        continue;
      }
      gtsam::Pose3_ T_w_c  = cam->poseEstimate.getPose();
      std::cout << "camera " << cam_idx << " pose: " << std::endl;
      print_pose(cam->poseEstimate.getPose());
      std::vector<gtsam::Point3> bp;
      std::vector<gtsam::Point2> ip;
      rb->getAttachedPoints(cam_idx, &bp, &ip);
      // now add points to graph
      gtsam::Expression<Cal3DS2U> cK(*cam->gtsamCameraModel);
      std::cout << "cam points: " << std::endl;
      std::cout << "pts=[" << std::endl;
      for (const auto i: irange(0ul, bp.size())) {
        gtsam::Point3_ p(bp[i]);
        std::cout << bp[i].x() << "," << bp[i].y() << "," << bp[i].z() << "," << ip[i].x() << ", " << ip[i].y() << ";" << std::endl;
        //std::cout << "transform to camera: T_c_w= " << std::endl << cam->poseEstimate.getPose().inverse() << std::endl;
        //std::cout << "transformed point: p: " << std::endl << cam->poseEstimate.getPose().inverse().transform_to(bp[i]) << std::endl;
        // P_A = transform_from(T_AB, P_B)
        gtsam::Point2_ xp = gtsam::project(gtsam::transform_to(T_w_c, gtsam::transform_from(T_w_b, p)));
        gtsam::Point2_ predict(cK, &Cal3DS2U::uncalibrate, xp);
        graph.addExpressionFactor(predict, ip[i], pixelNoise);
      }
      std::cout << "];" << std::endl;
    }
    gtsam::Values initialValues;
    pe = optimizeGraph(initialPose, initialValues, &graph);
    std::cout << "optimized graph pose T_w_b: " << std::endl;
    print_pose(pe.getPose());
    std::cout << "----------------- analysis of final pose -----" << std::endl;
    analyze_pose(cams, rb, pe.getPose());
    return (pe);
  }

  

  PoseEstimate
  InitialPoseGraph::estimateCameraPose(const CameraPtr &camera,
                                       const std::vector<gtsam::Point3> &wp,
                                       const std::vector<gtsam::Point2> &ip,
                                       const PoseEstimate &initialPose) const {
    PoseEstimate pe; // defaults to invalid
    if (wp.empty()) {
      return (pe);
    }
    gtsam::NonlinearFactorGraph   graph;
    auto pixelNoise = gtsam::noiseModel::Isotropic::Sigma(2, 1.0);
    boost::shared_ptr<gtsam::Cal3DS2> cam = camera->gtsamCameraModel;
    graph = gtsam::NonlinearFactorGraph();
    gtsam::Symbol P = gtsam::Symbol('P', 0); // pose symbol
    for (const auto i: boost::irange(0ul, wp.size())) {
      graph.push_back(boost::make_shared<ResectioningFactor>(
                         pixelNoise, P, cam, ip[i], wp[i]));
    }
    gtsam::Values initialValues;
    pe = optimizeGraph(initialPose, initialValues, &graph);
    return (pe);
  }

  PoseEstimate
  InitialPoseGraph::optimizeGraph(const gtsam::Pose3 &startPose,
                                  const gtsam::Values &startValues,
                                  gtsam::NonlinearFactorGraph *graph) const {
  	RandEng	randomEngine;
    RandDist distTrans(0, 10.0); // mu, sigma for translation
    RandDist distRot(0, M_PI);	 // mu, sigma for rotations
    RandGen	 rgt(randomEngine, distTrans);	 // random translation generator
    RandGen  rgr(randomEngine, distRot);	   // random angle generator
    gtsam::Pose3 pose = startPose;
    PoseEstimate bestPose(startPose);
    for (const auto i: irange(0, 50)) {
      PoseEstimate pe = try_optimization(pose, startValues, graph);
      if (pe.getError() < bestPose.getError()) {
        bestPose = pe;
      }
      if (bestPose.getError() < 10.0) {
        break;
      }
      pose = make_random_pose(&rgr, &rgt);
    }
    return (bestPose);
  }
  

}  // namespace
