/**
 * spaint: SLAMComponent.cpp
 * Copyright (c) Torr Vision Group, University of Oxford, 2016. All rights reserved.
 */

#include "pipelinecomponents/SLAMComponentWithScoreForest.h"

#include <algorithm>
#include <tuple>
#include <random>

#include <boost/timer/timer.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>

#include <omp.h>

#include <DatasetRGBDInfiniTAM.hpp>

#include <libalglib/optimization.h>

#include "ITMLib/Trackers/ITMTrackerFactory.h"

#include "ocv/OpenCVUtil.h"
#include "randomforest/cuda/GPUForest_CUDA.h"
#include "randomforest/cuda/GPURansac_CUDA.h"
#include "util/PosePersister.h"

#include "Helpers.hpp"
#include "tvgutil/filesystem/PathFinder.h"
#include "tvgutil/timing/TimeUtil.h"

using namespace InputSource;
using namespace ITMLib;
using namespace ORUtils;
using namespace RelocLib;
using namespace tvgutil;

//#define ENABLE_TIMERS
//#define VISUALIZE_INLIERS
#define SAVE_RELOC_POSES

namespace spaint
{

//#################### CONSTRUCTORS ####################

SLAMComponentWithScoreForest::SLAMComponentWithScoreForest(
    const SLAMContext_Ptr& context, const std::string& sceneID,
    const ImageSourceEngine_Ptr& imageSourceEngine, TrackerType trackerType,
    const std::vector<std::string>& trackerParams, MappingMode mappingMode,
    TrackingMode trackingMode) :
    SLAMComponent(context, sceneID, imageSourceEngine, trackerType,
        trackerParams, mappingMode, trackingMode)
{
  m_dataset.reset(
      new DatasetRGBDInfiniTAM(
//          "/home/tcavallari/code/scoreforests/apps/TrainAndTest/SettingsDatasetRGBDInfiniTAMDesk.yml",
          "/home/tcavallari/code/scoreforests/apps/TrainAndTest/SettingsDatasetRGBD7ScenesChessOnline.yml",
//          "/home/tcavallari/code/scoreforests/apps/TrainAndTest/SettingsDatasetRGBD7ScenesOfficeOnline.yml",
          "/media/data/", 5, 1.0, "DFBP", true, 0, false, 42));

  m_dataset->LoadForest();
//  m_dataset->ResetNodeAndLeaves();

  m_featureExtractor =
      FeatureCalculatorFactory::make_rgbd_patch_feature_calculator(
          ITMLib::ITMLibSettings::DEVICE_CUDA);
  m_featureImage.reset(new RGBDPatchFeatureImage(Vector2i(0, 0), true, true)); // Dummy size just to allocate the container
  m_predictionsImage.reset(
      new GPUForestPredictionsImage(Vector2i(0, 0), true, true)); // Dummy size just to allocate the container

  m_gpuForest.reset(new GPUForest_CUDA(*m_dataset->GetForest()));
  m_gpuForest->reset_predictions();

  m_gpuRansac.reset(new GPURansac_CUDA());

  // Set params as in scoreforests
  m_kInitRansac = 1024;
  m_nbPointsForKabschBoostrap = 3;
  m_useAllModesPerLeafInPoseHypothesisGeneration = true;
  m_checkMinDistanceBetweenSampledModes = true;
  m_minDistanceBetweenSampledModes = 0.3f;
//  m_checkRigidTransformationConstraint = false; // Speeds up a lot, was true in scoreforests
  m_checkRigidTransformationConstraint = true;
  m_translationErrorMaxForCorrectPose = 0.05f;
  m_batchSizeRansac = 500;
  m_trimKinitAfterFirstEnergyComputation = 64;
  m_poseUpdate = true; // original
//  m_poseUpdate = false; // faster, might be OK
  m_usePredictionCovarianceForPoseOptimization = true; // original implementation
//  m_usePredictionCovarianceForPoseOptimization = false;
  m_poseOptimizationInlierThreshold = 0.2f;

  // Additional stuff
  m_maxNbModesPerLeaf = 10; //5-10 seem to be enough

  // Refinement ICP tracker
  const Settings_CPtr& settings = m_context->get_settings();
  const SLAMState_Ptr& slamState = m_context->get_slam_state(m_sceneID);
  const Vector2i& depthImageSize = slamState->get_depth_image_size();
  const Vector2i& rgbImageSize = slamState->get_rgb_image_size();
  const SpaintVoxelScene_Ptr& voxelScene = slamState->get_voxel_scene();

#ifdef SAVE_RELOC_POSES
  const std::string refineParams =
      "type=extended,levels=rrbb,minstep=1e-4,outlierSpaceC=0.1,outlierSpaceF=0.004,numiterC=20,numiterF=20,tukeyCutOff=8,framesToSkip=20,framesToWeight=50,failureDec=20.0";

  m_refineTracker.reset(
      ITMTrackerFactory<SpaintVoxel, ITMVoxelIndex>::Instance().Make(
          refineParams.c_str(), rgbImageSize, depthImageSize, settings.get(),
          m_lowLevelEngine.get(), NULL, voxelScene.get()));

  const std::string poses_folder =
      m_context->get_tag().empty() ?
          TimeUtil::get_iso_timestamp() : m_context->get_tag();

  m_sequentialPathGenerator.reset(
      SequentialPathGenerator(
          find_subdir_from_executable("reloc_poses") / poses_folder));

  std::cout << "Saving relocalization poses in: "
      << m_sequentialPathGenerator->get_base_dir() << std::endl;
  boost::filesystem::create_directories(
      m_sequentialPathGenerator->get_base_dir());
#endif
}

//#################### DESTRUCTOR ####################
SLAMComponentWithScoreForest::~SLAMComponentWithScoreForest()
{
}

//#################### PROTECTED MEMBER FUNCTIONS ####################

SLAMComponent::TrackingResult SLAMComponentWithScoreForest::process_relocalisation(
    TrackingResult trackingResult)
{
  const SLAMState_Ptr& slamState = m_context->get_slam_state(m_sceneID);
  const ITMFloatImage_Ptr inputDepthImage(
      new ITMFloatImage(slamState->get_depth_image_size(), true, true));
  inputDepthImage->SetFrom(slamState->get_view()->depth,
      ORUtils::MemoryBlock<float>::CUDA_TO_CUDA);

  const ITMUChar4Image_Ptr inputRGBImage(
      new ITMUChar4Image(slamState->get_rgb_image_size(), true, true));
  inputRGBImage->SetFrom(slamState->get_view()->rgb,
      ORUtils::MemoryBlock<Vector4u>::CUDA_TO_CUDA);

  const TrackingState_Ptr& trackingState = slamState->get_tracking_state();

  const View_Ptr& view = slamState->get_view();

  const Vector4f depthIntrinsics =
      view->calib.intrinsics_d.projectionParamsSimple.all;

  if (trackingResult == TrackingResult::TRACKING_FAILED)
  {
#ifdef ENABLE_TIMERS
    boost::timer::auto_cpu_timer t(6,
        "relocalization, overall: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
#endif

    if (m_lowLevelEngine->CountValidDepths(inputDepthImage.get())
        < m_gpuRansac->get_min_nb_required_points())
    {
      std::cout
          << "Number of valid depth pixels insufficient to perform relocalization."
          << std::endl;

      if (m_sequentialPathGenerator)
      {
        m_sequentialPathGenerator->increment_index();
      }
      return trackingResult;
    }

    compute_features(inputRGBImage, inputDepthImage, depthIntrinsics);
    evaluate_forest();
    boost::optional<PoseCandidate> pose_candidate = m_gpuRansac->estimate_pose(
        m_featureImage, m_predictionsImage);

    if (pose_candidate)
    {
//      std::cout << "The final pose is:" << pose_candidate->cameraPose
//          << "\n and has " << pose_candidate->inliers.size() << " inliers."
//          << std::endl;

#ifdef VISUALIZE_INLIERS
      cv::Mat inliers = cv::Mat::zeros(
          cv::Size(m_featureImage->noDims.width, m_featureImage->noDims.height),
          CV_32FC1);
      inliers.setTo(std::numeric_limits<float>::quiet_NaN());

      for (size_t i = 0; i < pose_candidate->inliers.size(); ++i)
      {
        int idx = pose_candidate->inliers[i].linearIdx;
        float energy = pose_candidate->inliers[i].energy;

        int x = idx % m_featureImage->noDims.width;
        int y = idx / m_featureImage->noDims.width;

        inliers.at<float>(cv::Point(x, y)) = energy;
      }

      double min, max;
      cv::minMaxIdx(inliers, &min, &max);
      std::cout << "Min energy: " << min << " - MAx energy: " << max
      << std::endl;

      cv::normalize(inliers, inliers, 0.0, 1.0, cv::NORM_MINMAX);
      inliers = 1.f - inliers;

      cv::imshow("Inliers mask", inliers);
      cv::waitKey(1);
#endif

      trackingState->pose_d->SetInvM(pose_candidate->cameraPose);

      const VoxelRenderState_Ptr& liveVoxelRenderState =
          slamState->get_live_voxel_render_state();
      const SpaintVoxelScene_Ptr& voxelScene = slamState->get_voxel_scene();
      const bool resetVisibleList = true;
      m_denseVoxelMapper->UpdateVisibleList(view.get(), trackingState.get(),
          voxelScene.get(), liveVoxelRenderState.get(), resetVisibleList);
      prepare_for_tracking(TRACK_VOXELS);
#ifdef SAVE_RELOC_POSES
      m_refineTracker->TrackCamera(trackingState.get(), view.get());
#else
      m_trackingController->Track(trackingState.get(), view.get());
#endif
      trackingResult = trackingState->trackerResult;

//      std::cout << "Refinement: "
//          << (trackingState->trackerResult == TrackingResult::TRACKING_GOOD ?
//              "SUCCESS" : "FAIL") << "\n Refined pose:\n"
//          << trackingState->pose_d->GetInvM() << std::endl;

      if (m_sequentialPathGenerator)
      {
        // Save poses
        PosePersister::save_pose_on_thread(pose_candidate->cameraPose,
            m_sequentialPathGenerator->make_path("pose-%06i.reloc.txt"));
        PosePersister::save_pose_on_thread(trackingState->pose_d->GetInvM(),
            m_sequentialPathGenerator->make_path("pose-%06i.icp.txt"));

        const Matrix4f final_pose =
            trackingResult == TrackingResult::TRACKING_GOOD ?
                trackingState->pose_d->GetInvM() : pose_candidate->cameraPose;

        PosePersister::save_pose_on_thread(final_pose,
            m_sequentialPathGenerator->make_path("pose-%06i.final.txt"));
      }

#ifdef SAVE_RELOC_POSES
      trackingResult = TrackingResult::TRACKING_POOR;
#endif
    }
    else
    {
      std::cout << "Cannot estimate a pose candidate." << std::endl;

      if (m_sequentialPathGenerator)
      {
        // Save dummy poses
        Matrix4f invalid_pose;
        invalid_pose.setValues(std::numeric_limits<float>::quiet_NaN());

        PosePersister::save_pose_on_thread(invalid_pose,
            m_sequentialPathGenerator->make_path("pose-%06i.reloc.txt"));
        PosePersister::save_pose_on_thread(invalid_pose,
            m_sequentialPathGenerator->make_path("pose-%06i.icp.txt"));
        PosePersister::save_pose_on_thread(invalid_pose,
            m_sequentialPathGenerator->make_path("pose-%06i.final.txt"));
      }
    }

    if (m_sequentialPathGenerator)
    {
      m_sequentialPathGenerator->increment_index();
    }
  }
  else if (trackingResult == TrackingResult::TRACKING_GOOD)
  {
    Matrix4f invCameraPose = trackingState->pose_d->GetInvM();
    compute_features(inputRGBImage, inputDepthImage, depthIntrinsics,
        invCameraPose);

#ifdef ENABLE_TIMERS
    boost::timer::auto_cpu_timer t(6,
        "add features to forest: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
#endif

    m_gpuForest->add_features_to_forest(m_featureImage);
  }

  return trackingResult;
}

//#################### PROTECTED MEMBER FUNCTIONS ####################

void SLAMComponentWithScoreForest::compute_features(
    const ITMUChar4Image_CPtr &inputRgbImage,
    const ITMFloatImage_CPtr &inputDepthImage, const Vector4f &depthIntrinsics)
{
  Matrix4f identity;
  identity.setIdentity();

  compute_features(inputRgbImage, inputDepthImage, depthIntrinsics, identity);
}

void SLAMComponentWithScoreForest::compute_features(
    const ITMUChar4Image_CPtr &inputRgbImage,
    const ITMFloatImage_CPtr &inputDepthImage, const Vector4f &depthIntrinsics,
    const Matrix4f &invCameraPose)
{
#ifdef ENABLE_TIMERS
  boost::timer::auto_cpu_timer t(6,
      "computing features on the GPU: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
#endif
  m_featureExtractor->ComputeFeature(inputRgbImage, inputDepthImage,
      depthIntrinsics, m_featureImage, invCameraPose);

}

void SLAMComponentWithScoreForest::evaluate_forest()
{
#ifdef ENABLE_TIMERS
  boost::timer::auto_cpu_timer t(6,
      "evaluating forest on the GPU: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
#endif
  m_gpuForest->evaluate_forest(m_featureImage, m_predictionsImage);
}

//void SLAMComponentWithScoreForest::generate_pose_candidates(
//    std::vector<PoseCandidate> &poseCandidates)
//{
//  const int nbThreads = 12;
//
//  poseCandidates.reserve(m_kInitRansac);
//
//  std::vector<std::mt19937> engs(nbThreads);
//  for (int i = 0; i < nbThreads; ++i)
//  {
//    engs[i].seed(static_cast<unsigned int>(i + 1));
//  }
//
//  omp_set_num_threads(nbThreads);
//
////  std::cout << "Generating pose candidates Kabsch" << std::endl;
//#pragma omp parallel for
//  for (size_t i = 0; i < m_kInitRansac; ++i)
//  {
//    int threadId = omp_get_thread_num();
//    PoseCandidate candidate;
//
//    if (hypothesize_pose(candidate, engs[threadId]))
//    {
//      if (!candidate.inliers.empty()) // Has some inliers
//      {
//        candidate.cameraId = i;
//
//#pragma omp critical
//        poseCandidates.emplace_back(std::move(candidate));
//      }
//    }
//  }
//}
//
//bool SLAMComponentWithScoreForest::hypothesize_pose(PoseCandidate &res,
//    std::mt19937 &eng)
//{
//  Eigen::MatrixXf worldPoints(3, m_nbPointsForKabschBoostrap);
//  Eigen::MatrixXf localPoints(3, m_nbPointsForKabschBoostrap);
//
//  std::uniform_int_distribution<int> col_index_generator(0,
//      m_featureImage->noDims.width - 1);
//  std::uniform_int_distribution<int> row_index_generator(0,
//      m_featureImage->noDims.height - 1);
//
//  const RGBDPatchFeature *patchFeaturesData = m_featureImage->GetData(
//      MEMORYDEVICE_CPU);
//  const GPUForestPrediction *predictionsData = m_predictionsImage->GetData(
//      MEMORYDEVICE_CPU);
//
//  bool foundIsometricMapping = false;
//  const int maxIterationsOuter = 20;
//  int iterationsOuter = 0;
//
//  while (!foundIsometricMapping && iterationsOuter < maxIterationsOuter)
//  {
//    ++iterationsOuter;
//    std::vector<std::tuple<int, int, int>> selectedPixelsAndModes;
//
//    const int maxIterationsInner = 6000;
//    int iterationsInner = 0;
//    while (selectedPixelsAndModes.size() != m_nbPointsForKabschBoostrap
//        && iterationsInner < maxIterationsInner)
//    {
//      ++iterationsInner;
//
//      const int x = col_index_generator(eng);
//      const int y = row_index_generator(eng);
//      const int linearFeatureIdx = y * m_featureImage->noDims.width + x;
//      const RGBDPatchFeature &selectedFeature =
//          patchFeaturesData[linearFeatureIdx];
//
//      if (selectedFeature.position.w < 0.f) // Invalid feature
//        continue;
//
//      const GPUForestPrediction &selectedPrediction =
//          predictionsData[linearFeatureIdx];
//
//      if (selectedPrediction.nbModes == 0)
//        continue;
//
//      int selectedModeIdx = 0;
//      if (m_useAllModesPerLeafInPoseHypothesisGeneration)
//      {
//        std::uniform_int_distribution<int> mode_generator(0,
//            selectedPrediction.nbModes - 1);
//        selectedModeIdx = mode_generator(eng);
//      }
//
//      // This is the first pixel, check that the pixel colour corresponds with the selected mode
//      if (selectedPixelsAndModes.empty())
//      {
//        const Vector3u colourDiff = selectedFeature.colour.toVector3().toUChar()
//            - selectedPrediction.modes[selectedModeIdx].colour;
//        const bool consistentColour = abs(colourDiff.x) <= 30
//            && abs(colourDiff.y) <= 30 && abs(colourDiff.z) <= 30;
//
//        if (!consistentColour)
//          continue;
//      }
//
//      // if (false)
//      if (m_checkMinDistanceBetweenSampledModes)
//      {
//        const Vector3f worldPt =
//            selectedPrediction.modes[selectedModeIdx].position;
//
//        // Check that this mode is far enough from the other modes
//        bool farEnough = true;
//
//        for (size_t idxOther = 0; idxOther < selectedPixelsAndModes.size();
//            ++idxOther)
//        {
//          int xOther, yOther, modeIdxOther;
//          std::tie(xOther, yOther, modeIdxOther) =
//              selectedPixelsAndModes[idxOther];
//
//          const int linearIdxOther = yOther * m_featureImage->noDims.width
//              + xOther;
//          const GPUForestPrediction &predOther = predictionsData[linearIdxOther];
//
//          Vector3f worldPtOther = predOther.modes[modeIdxOther].position;
//
//          float distOther = length(worldPtOther - worldPt);
//          if (distOther < m_minDistanceBetweenSampledModes)
//          {
//            farEnough = false;
//            break;
//          }
//        }
//
//        if (!farEnough)
//          continue;
//      }
//
//      // isometry?
////       if (false)
//      // if (true)
//      if (m_checkRigidTransformationConstraint)
//      {
//        bool violatesConditions = false;
//
//        for (size_t m = 0;
//            m < selectedPixelsAndModes.size() && !violatesConditions; ++m)
//        {
//          int xFirst, yFirst, modeIdxFirst;
//          std::tie(xFirst, yFirst, modeIdxFirst) = selectedPixelsAndModes[m];
//
//          const int linearIdxOther = yFirst * m_featureImage->noDims.width
//              + xFirst;
//          const GPUForestPrediction &predFirst = predictionsData[linearIdxOther];
//
//          const Vector3f worldPtFirst = predFirst.modes[modeIdxFirst].position;
//          const Vector3f worldPtCur =
//              selectedPrediction.modes[selectedModeIdx].position;
//
//          float distWorld = length(worldPtFirst - worldPtCur);
//
//          const Vector3f localPred =
//              patchFeaturesData[linearIdxOther].position.toVector3();
//          const Vector3f localCur = selectedFeature.position.toVector3();
//
//          float distLocal = length(localPred - localCur);
//
//          if (distLocal < m_minDistanceBetweenSampledModes)
//            violatesConditions = true;
//
//          if (std::abs(distLocal - distWorld)
//              > 0.5f * m_translationErrorMaxForCorrectPose)
//          {
//            violatesConditions = true;
//          }
//        }
//
//        if (violatesConditions)
//          continue;
//      }
//
//      selectedPixelsAndModes.push_back(
//          std::tuple<int, int, int>(x, y, selectedModeIdx));
////      iterationsInner = 0;
//    }
//
////    std::cout << "Inner iterations: " << iterationsInner << std::endl;
//
//    // Reached limit of iterations
//    if (selectedPixelsAndModes.size() != m_nbPointsForKabschBoostrap)
//      return false;
//
//    // Populate resulting pose
//    res.inliers.clear();
//    for (size_t s = 0; s < selectedPixelsAndModes.size(); ++s)
//    {
//      int x, y, modeIdx;
//      std::tie(x, y, modeIdx) = selectedPixelsAndModes[s];
//      const int linearIdx = y * m_featureImage->noDims.width + x;
//      const GPUForestPrediction &pred = predictionsData[linearIdx];
//
//      Eigen::VectorXf localPt = Eigen::Map<const Eigen::Vector3f>(
//          patchFeaturesData[linearIdx].position.v);
//
//      Eigen::VectorXf worldPt = Eigen::Map<const Eigen::Vector3f>(
//          pred.modes[modeIdx].position.v);
//
//      for (int idx = 0; idx < 3; ++idx)
//      {
//        localPoints(idx, s) = localPt(idx);
//        worldPoints(idx, s) = worldPt(idx);
//      }
//
//      res.inliers.push_back(PoseCandidate::Inlier
//      { linearIdx, modeIdx, 0.f });
//    }
//
//    {
////#ifdef ENABLE_TIMERS
////      boost::timer::auto_cpu_timer t(6,
////          "kabsch: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
////#endif
//      Eigen::Map<Eigen::Matrix4f>(res.cameraPose.m) = Helpers::Kabsch(
//          localPoints, worldPoints);
//    }
//
//    foundIsometricMapping = true;
//
//    res.energy = 0.f;
//    res.cameraId = -1;
//  }
//
//  if (iterationsOuter < maxIterationsOuter)
//    return true;
//
//  return false;
//}
//
//boost::optional<SLAMComponentWithScoreForest::PoseCandidate> SLAMComponentWithScoreForest::estimate_pose()
//{
//  std::mt19937 random_engine;
//
//  m_featureImage->UpdateHostFromDevice(); // Need the features on the host for now
//  m_predictionsImage->UpdateHostFromDevice(); // Also the predictions
//
//  std::vector<PoseCandidate> candidates;
//
//  {
//#ifdef ENABLE_TIMERS
//    boost::timer::auto_cpu_timer t(6,
//        "generating initial candidates: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
//#endif
//    generate_pose_candidates(candidates);
//  }
//
////  std::cout << "Generated " << candidates.size() << " initial candidates."
////      << std::endl;
//
//  if (m_trimKinitAfterFirstEnergyComputation < candidates.size())
//  {
//#ifdef ENABLE_TIMERS
//    boost::timer::auto_cpu_timer t(6,
//        "first trim: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
//#endif
//    size_t nbSamplesPerCamera = candidates[0].inliers.size();
//    std::vector<Vector2i> sampledPixelIdx;
//    std::vector<bool> dummy_vector;
//
//    {
//#ifdef ENABLE_TIMERS
//      boost::timer::auto_cpu_timer t(6,
//          "sample pixels: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
//#endif
//      sample_pixels_for_ransac(dummy_vector, sampledPixelIdx, random_engine,
//          m_batchSizeRansac);
//    }
//
//    {
//#ifdef ENABLE_TIMERS
//      boost::timer::auto_cpu_timer t(6,
//          "update inliers: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
//#endif
//      update_inliers_for_optimization(sampledPixelIdx, candidates);
//    }
//
//    {
//#ifdef ENABLE_TIMERS
//      boost::timer::auto_cpu_timer t(6,
//          "compute and sort energies: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
//#endif
//      compute_and_sort_energies(candidates);
//    }
//
//    candidates.erase(
//        candidates.begin() + m_trimKinitAfterFirstEnergyComputation,
//        candidates.end());
//
//    if (m_trimKinitAfterFirstEnergyComputation > 1)
//    {
//      for (size_t p = 0; p < candidates.size(); ++p)
//      {
//        auto &samples = candidates[p].inliers;
//        if (samples.size() > nbSamplesPerCamera)
//          samples.erase(samples.begin() + nbSamplesPerCamera, samples.end());
//      }
//    }
//  }
//
//  //  std::cout << candidates.size() << " candidates remaining." << std::endl;
//  //  std::cout << "Premptive RANSAC" << std::endl;
//
//#ifdef ENABLE_TIMERS
//  boost::timer::auto_cpu_timer t(6,
//      "ransac: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
//#endif
//
//  std::vector<bool> maskSampledPixels(m_featureImage->dataSize, false);
//
//  float iteration = 0.0f;
//
//  while (candidates.size() > 1)
//  {
//    //    boost::timer::auto_cpu_timer t(
//    //        6, "ransac iteration: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
//    ++iteration;
//    //    std::cout << candidates.size() << " camera remaining" << std::endl;
//
//    std::vector<Vector2i> sampledPixelIdx;
//    sample_pixels_for_ransac(maskSampledPixels, sampledPixelIdx, random_engine,
//        m_batchSizeRansac);
//
//    //    std::cout << "Updating inliers to each pose candidate..." << std::endl;
//    update_inliers_for_optimization(sampledPixelIdx, candidates);
//
//    if (m_poseUpdate)
//    {
//      update_candidate_poses(candidates);
//    }
//
//    compute_and_sort_energies(candidates);
//
//    // Remove half of the candidates with the worse energies
//    candidates.erase(candidates.begin() + candidates.size() / 2,
//        candidates.end());
//  }
//
//  return !candidates.empty() ? candidates[0] : boost::optional<PoseCandidate>();
//}
//
//void SLAMComponentWithScoreForest::sample_pixels_for_ransac(
//    std::vector<bool> &maskSampledPixels,
//    std::vector<Vector2i> &sampledPixelIdx, std::mt19937 &eng, int batchSize)
//{
//  std::uniform_int_distribution<int> col_index_generator(0,
//      m_featureImage->noDims.width - 1);
//  std::uniform_int_distribution<int> row_index_generator(0,
//      m_featureImage->noDims.height - 1);
//
//  const RGBDPatchFeature *patchFeaturesData = m_featureImage->GetData(
//      MEMORYDEVICE_CPU);
//  const GPUForestPrediction *predictionsData = m_predictionsImage->GetData(
//      MEMORYDEVICE_CPU);
//
//  for (int i = 0; i < batchSize; ++i)
//  {
//    bool validIndex = false;
//    int innerIterations = 0;
//
//    while (!validIndex && innerIterations++ < 50)
//    {
//      const Vector2i s(col_index_generator(eng), row_index_generator(eng));
//      const int linearIdx = s.y * m_featureImage->noDims.width + s.x;
//
//      if (patchFeaturesData[linearIdx].position.w >= 0.f)
//      {
//        const GPUForestPrediction &selectedPrediction =
//            predictionsData[linearIdx];
//
//        if (selectedPrediction.nbModes > 0)
//        {
//          validIndex = maskSampledPixels.empty()
//              || !maskSampledPixels[linearIdx];
//
//          if (validIndex)
//          {
//            sampledPixelIdx.push_back(s);
//
//            if (!maskSampledPixels.empty())
//              maskSampledPixels[linearIdx] = true;
//          }
//        }
//      }
//    }
//
//    if (!validIndex)
//    {
//      std::cout << "Couldn't sample a valid pixel. Returning "
//          << sampledPixelIdx.size() << "/" << batchSize << std::endl;
//      break;
//    }
//  }
//}
//
//void SLAMComponentWithScoreForest::update_inliers_for_optimization(
//    const std::vector<Vector2i> &sampledPixelIdx,
//    std::vector<PoseCandidate> &poseCandidates) const
//{
//#pragma omp parallel for
//  for (size_t p = 0; p < poseCandidates.size(); ++p)
//  {
//    auto &inliers = poseCandidates[p].inliers;
//
//    // add all the samples as inliers
//    for (size_t s = 0; s < sampledPixelIdx.size(); ++s)
//    {
//      const Vector2i &sample = sampledPixelIdx[s];
//      const int linearIdx = sample.y * m_featureImage->noDims.width + sample.x;
//
//      inliers.push_back(PoseCandidate::Inlier
//      { linearIdx, -1, 0.f });
//    }
//  }
//}
//
//void SLAMComponentWithScoreForest::compute_and_sort_energies(
//    std::vector<PoseCandidate> &poseCandidates) const
//{
////  int nbPoseProcessed = 0;
//#pragma omp parallel for
//  for (size_t p = 0; p < poseCandidates.size(); ++p)
//  {
//    //#pragma omp critical
//    //    {
//    //      //#pragma omp flush(nbPoseProcessed)
//    //      //      Helpers::displayPercentage(nbPoseProcessed++, poseCandidates.size());
//    //    }
//
//    PoseCandidate &candidate = poseCandidates[p];
//    candidate.energy = compute_pose_energy(candidate.cameraPose,
//        candidate.inliers);
//  }
//
//  // Sort by ascending energy
//  std::sort(poseCandidates.begin(), poseCandidates.end(),
//      [] (const PoseCandidate &a, const PoseCandidate &b)
//      { return a.energy < b.energy;});
//}
//
//float SLAMComponentWithScoreForest::compute_pose_energy(
//    const Matrix4f &candidateCameraPose,
//    std::vector<PoseCandidate::Inlier> &inliersIndices) const
//{
//  float totalEnergy = 0.0f;
//
//  const RGBDPatchFeature *patchFeaturesData = m_featureImage->GetData(
//      MEMORYDEVICE_CPU);
//  const GPUForestPrediction *predictionsData = m_predictionsImage->GetData(
//      MEMORYDEVICE_CPU);
//
//  for (size_t s = 0; s < inliersIndices.size(); ++s)
//  {
//    const int linearIdx = inliersIndices[s].linearIdx;
//    const Vector3f localPixel =
//        patchFeaturesData[linearIdx].position.toVector3();
//    const Vector3f projectedPixel = candidateCameraPose * localPixel;
//
//    const GPUForestPrediction &pred = predictionsData[linearIdx];
//
//    // eval individual energy
//    float energy;
//    int argmax = pred.get_best_mode_and_energy(projectedPixel, energy);
//
//    // Has at least a valid mode
//    if (argmax < 0)
//    {
//      // should have not been inserted in the inlier set
//      std::cout << "prediction " << linearIdx
//          << " has negative argmax, nbModes: " << pred.nbModes << std::endl;
//      for (int i = 0; i < pred.nbModes; ++i)
//      {
//        auto &mode = pred.modes[i];
//        std::cout << "Mode " << i << ": inliers: " << mode.nbInliers
//            << "\npos: " << mode.position << "\ncol: " << mode.colour
//            << "\ndet: " << mode.determinant << "\ninvcov: "
//            << mode.positionInvCovariance << "\n" << std::endl;
//      }
//      throw std::runtime_error("prediction has no valid modes");
//    }
//
//    if (pred.modes[argmax].nbInliers == 0)
//    {
//      // the original implementation had a simple continue
//      std::cout << "mode has no inliers" << std::endl;
//      throw std::runtime_error("mode has no inliers");
//    }
//
//    energy /= static_cast<float>(pred.nbModes);
//    energy /= static_cast<float>(pred.modes[argmax].nbInliers);
//
//    if (energy < 1e-6f)
//      energy = 1e-6f;
//    energy = -log10f(energy);
//
//    inliersIndices[s].energy = energy;
//    inliersIndices[s].modeIdx = argmax;
//    totalEnergy += energy;
//  }
//
//  return totalEnergy / static_cast<float>(inliersIndices.size());
//}
//
//void SLAMComponentWithScoreForest::update_candidate_poses(
//    std::vector<PoseCandidate> &poseCandidates) const
//{
////  int nbUpdated = 0;
//#pragma omp parallel for
//  for (size_t i = 0; i < poseCandidates.size(); ++i)
//  {
//    if (update_candidate_pose(poseCandidates[i]))
//    {
//      //#pragma omp atomic
//      //      ++nbUpdated;
//    }
//  }
//  //  std::cout << nbUpdated << "/" << poseCandidates.size() << " updated cameras" << std::endl;
//}
//
//namespace
//{
//struct PointForLM
//{
//  Vector3f point;
//  GPUForestMode mode;
//};
//
//typedef std::vector<PointForLM> PointsForLM;
//
////struct PointsForLM
////{
////  std::vector<Vector3f, GPUForestMode>> pts;
//////  PointsForLM(int nbPts) :
//////      pts(nbPts), blurred_img(NULL)
//////  {
//////  }
//////  ~PointsForLM()
//////  {
//////  }
//////  std::vector<
//////      std::pair<std::vector<Eigen::VectorXd>,
//////          std::vector<PredictedGaussianMean *>>> pts;
//////  GaussianAggregatedRGBImage *blurred_img;
////};
//
//static double EnergyForContinuous3DOptimizationUsingFullCovariance(
//    const PointsForLM &pts, const ORUtils::SE3Pose &candidateCameraPose)
//{
//  double res = 0.0;
//
//  for (size_t i = 0; i < pts.size(); ++i)
//  {
//    const PointForLM &pt = pts[i];
//    const Vector3f transformedPt = candidateCameraPose.GetM() * pt.point;
//    const Vector3f diff = transformedPt - pt.mode.position;
//    const double err = dot(diff, pt.mode.positionInvCovariance * diff); // Mahalanobis sqr distance
//    res += err;
//  }
//
//  return res;
//}
//
//static void Continuous3DOptimizationUsingFullCovariance(
//    const alglib::real_1d_array &ksi, alglib::real_1d_array &fi, void *ptr)
//{
//  const PointsForLM *ptsLM = reinterpret_cast<PointsForLM *>(ptr);
//  const ORUtils::SE3Pose testPose(ksi[0], ksi[1], ksi[2], ksi[3], ksi[4],
//      ksi[5]);
//
//  fi[0] = EnergyForContinuous3DOptimizationUsingFullCovariance(*ptsLM,
//      testPose);
//}
//
///***************************************************/
///* Routines to optimize the sum of 3D L2 distances */
///***************************************************/
//
////static double EnergyForContinuous3DOptimizationUsingL2(
////    std::vector<
////        std::pair<std::vector<Eigen::VectorXd>,
////            std::vector<PredictedGaussianMean *>>> &pts,
////    Eigen::MatrixXd &candidateCameraPoseD)
////{
////  double res = 0.0;
////  Eigen::VectorXd diff = Eigen::VectorXd::Zero(3);
////  Eigen::VectorXd transformedPthomogeneous(4);
////
////  for (size_t i = 0; i < pts.size(); ++i)
////  {
////    Helpers::Rigid3DTransformation(candidateCameraPoseD, pts[i].first[0],
////        transformedPthomogeneous);
////
////    for (int p = 0; p < 3; ++p)
////    {
////      diff(p) = transformedPthomogeneous(p) - pts[i].second[0]->_mean(p);
////    }
////
////    double err = diff.norm();
////    err *= err;
////    res += err;
////  }
////  return res;
////}
////
////static void Continuous3DOptimizationUsingL2(const alglib::real_1d_array &x,
////    alglib::real_1d_array &fi, void *ptr)
////{
////  PointsForLM *ptsLM = reinterpret_cast<PointsForLM *>(ptr);
////
////  std::vector<
////      std::pair<std::vector<Eigen::VectorXd>,
////          std::vector<PredictedGaussianMean *>>> &pts = ptsLM->pts;
////  // integrate the size of the clusters?
////  Eigen::VectorXd ksi(6);
////  memcpy(ksi.data(), x.getcontent(), 6 * sizeof(double));
////  /*for (int i = 0 ; i < 6 ; ++i)
////   {
////   ksi(i) = x[i];
////   }*/
////  Eigen::MatrixXd updatedCandidateCameraPoseD =
////      Helpers::LieAlgebraToLieGroupSE3(ksi);
////
////  fi[0] = EnergyForContinuous3DOptimizationUsingL2(pts,
////      updatedCandidateCameraPoseD);
////  return;
////}
//static void call_after_each_step(const alglib::real_1d_array &x, double func,
//    void *ptr)
//{
//  return;
//}
//}
//
//bool SLAMComponentWithScoreForest::update_candidate_pose(
//    PoseCandidate &poseCandidate) const
//{
//  const RGBDPatchFeature *patchFeaturesData = m_featureImage->GetData(
//      MEMORYDEVICE_CPU);
//  const GPUForestPrediction *predictionsData = m_predictionsImage->GetData(
//      MEMORYDEVICE_CPU);
//
//  ORUtils::SE3Pose candidateCameraPose(poseCandidate.cameraPose);
//  std::vector<PoseCandidate::Inlier> &inliers = poseCandidate.inliers;
//
//  PointsForLM ptsForLM;
//  for (size_t inlierIdx = 0; inlierIdx < inliers.size(); ++inlierIdx)
//  {
//    const PoseCandidate::Inlier &inlier = inliers[inlierIdx];
//    const Vector3f inlierCameraPosition =
//        patchFeaturesData[inlier.linearIdx].position.toVector3();
//    const Vector3f inlierWorldPosition = candidateCameraPose.GetM()
//        * inlierCameraPosition;
//    const GPUForestPrediction &prediction = predictionsData[inlier.linearIdx];
//
//    PointForLM ptLM;
//    // The assumption is that the inlier is valid (checked before)
//    ptLM.point = inlierCameraPosition;
//
//    // Find the best mode
//    // (do not rely on the one stored in the inlier because for the randomly sampled inliers it's not set)
//    int bestModeIdx = prediction.get_best_mode(inlierWorldPosition);
//    if (bestModeIdx < 0 || bestModeIdx >= prediction.nbModes)
//      throw std::runtime_error("best mode idx invalid."); // should have not been selected as inlier
//    ptLM.mode = prediction.modes[bestModeIdx];
//
//    if (length(ptLM.mode.position - inlierWorldPosition)
//        < m_poseOptimizationInlierThreshold)
//      ptsForLM.push_back(ptLM);
//  }
//
//  // Continuous optimization
//  if (ptsForLM.size() > 3)
//  {
//    const float *ksiF = candidateCameraPose.GetParams();
//    double ksiD[6];
//
//    // Cast to double
//    for (int i = 0; i < 6; ++i)
//      ksiD[i] = ksiF[i];
//
//    alglib::real_1d_array ksi_;
//    ksi_.setcontent(6, ksiD);
//
////    Eigen::MatrixXd candidateCameraPoseD = candidateCameraPose.cast<double>();
////
////    Eigen::VectorXd ksivd = Helpers::LieGroupToLieAlgebraSE3(
////        candidateCameraPoseD);
////
////    for (int i = 0; i < 6; ++i)
////    {
////      ksiD[i] = ksivd(i);
////    }
////
////    ksi_.setcontent(6, ksiD);
//
//    alglib::minlmstate state;
//    alglib::minlmreport rep;
//
//    double differentiationStep = 0.0001;
//    alglib::minlmcreatev(6, 1, ksi_, differentiationStep, state);
//
//    double epsg = 0.000001;
//    double epsf = 0;
//    double epsx = 0;
//    alglib::ae_int_t maxits = 100;
//    alglib::minlmsetcond(state, epsg, epsf, epsx, maxits);
//
//    double energyBefore, energyAfter;
//    if (m_usePredictionCovarianceForPoseOptimization)
//    {
//      energyBefore = EnergyForContinuous3DOptimizationUsingFullCovariance(
//          ptsForLM, candidateCameraPose);
//      alglib::minlmoptimize(state, Continuous3DOptimizationUsingFullCovariance,
//          call_after_each_step, &ptsForLM);
//    }
//    else
//    {
//      throw std::runtime_error("Not updated yet");
////      energyBefore = EnergyForContinuous3DOptimizationUsingL2(ptsForLM.pts,
////          candidateCameraPoseD);
////      alglib::minlmoptimize(state, Continuous3DOptimizationUsingL2,
////          call_after_each_step, &ptsForLM);
//    }
//
//    alglib::minlmresults(state, ksi_, rep);
//
//    candidateCameraPose.SetFrom(ksi_[0], ksi_[1], ksi_[2], ksi_[3], ksi_[4],
//        ksi_[5]);
//
////    memcpy(ksiD, ksi_.getcontent(), sizeof(double) * 6);
////    for (int i = 0; i < 6; ++i)
////    {
////      ksivd(i) = ksiD[i];
////    }
////    Eigen::MatrixXd updatedCandidateCameraPoseD =
////        Helpers::LieAlgebraToLieGroupSE3(ksivd);
//
//    if (m_usePredictionCovarianceForPoseOptimization)
//    {
//      energyAfter = EnergyForContinuous3DOptimizationUsingFullCovariance(
//          ptsForLM, candidateCameraPose);
//    }
//    else
//    {
//      throw std::runtime_error("Not updated yet");
////      energyAfter = EnergyForContinuous3DOptimizationUsingL2(ptsForLM.pts,
////          updatedCandidateCameraPoseD);
//    }
//
//    if (energyAfter < energyBefore)
//    {
//      poseCandidate.cameraPose = candidateCameraPose.GetM();
//      return true;
//    }
//  }
//
//  ////////////////////////////
//
////  std::vector<std::pair<int, int>> &samples = std::get < 1 > (poseCandidate);
////
////  const RGBDPatchFeature *patchFeaturesData = m_featureImage->GetData(
////      MEMORYDEVICE_CPU);
////
////  PointsForLM ptsForLM(0);
////
////  for (int s = 0; s < samples.size(); ++s)
////  {
////    const int x = samples[s].first % m_featureImage->noDims.width;
////    const int y = samples[s].first / m_featureImage->noDims.width;
////    const int linearizedIdx = samples[s].first;
////
////    std::pair<std::vector<Eigen::VectorXd>, std::vector<PredictedGaussianMean *>> pt;
////
////    Eigen::VectorXf pixelLocalCoordinates = Eigen::Map<const Eigen::Vector4f>(
////        patchFeaturesData[linearizedIdx].position.v);
////
////    pt.first.push_back(pixelLocalCoordinates.cast<double>());
////    // Eigen::VectorXf  projectedPixel = candidateCameraPose * pixelLocalCoordinates;
////    Eigen::VectorXd projectedPixel = (candidateCameraPose
////        * pixelLocalCoordinates).cast<double>();
////
////    boost::shared_ptr<EnsemblePredictionGaussianMean> epgm =
////        m_featurePredictions[linearizedIdx];
////
////    int argmax = epgm->GetArgMax3D(projectedPixel, 0);
////    if (argmax == -1)
////      continue;
////    pt.second.push_back(epgm->_modes[argmax][0]);
////
////    if ((epgm->_modes[argmax][0]->_mean
////        - Helpers::ConvertWorldCoordinatesFromHomogeneousCoordinates(
////            projectedPixel)).norm() < 0.2)
////      ptsForLM.pts.push_back(pt);
////  }
//
//  return false;
//}

}
