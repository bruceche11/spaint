/**
 * spaint: BackgroundSubtractingObjectSegmenter.h
 * Copyright (c) Torr Vision Group, University of Oxford, 2016. All rights reserved.
 */

#ifndef H_SPAINT_BACKGROUNDSUBTRACTINGOBJECTSEGMENTER
#define H_SPAINT_BACKGROUNDSUBTRACTINGOBJECTSEGMENTER

#include "ColourAppearanceModel.h"
#include "Segmenter.h"
#include "../touch/TouchDetector.h"

namespace spaint {

/**
 * \brief An instance of this class can be used to segment an object that is placed in front of a static scene
 *        using background subtraction.
 */
class BackgroundSubtractingObjectSegmenter : public Segmenter
{
  //#################### TYPEDEFS ####################
private:
  typedef boost::shared_ptr<const ITMFloatImage> ITMFloatImage_CPtr;
  typedef boost::shared_ptr<const ITMLib::ITMLibSettings> ITMSettings_CPtr;
  typedef boost::shared_ptr<const ITMUChar4Image> ITMUChar4Image_CPtr;

  //#################### PRIVATE VARIABLES ####################
private:
  /** The colour appearance model to use to separate the user's hand from any object it's holding. */
  ColourAppearanceModel_Ptr m_handAppearanceModel;

  /** The touch detector to use to get the initial difference mask. */
  mutable TouchDetector_Ptr m_touchDetector;

  //#################### CONSTRUCTORS ####################
public:
  BackgroundSubtractingObjectSegmenter(const ITMSettings_CPtr& itmSettings, const TouchSettings_Ptr& touchSettings, const View_CPtr& view);

  //#################### PUBLIC MEMBER FUNCTIONS ####################
public:
  /** Override */
  virtual void reset();

  /** Override */
  virtual ITMUCharImage_CPtr segment(const ORUtils::SE3Pose& pose, const RenderState_CPtr& renderState) const;

  /** Override */
  virtual ITMUChar4Image_Ptr train(const ORUtils::SE3Pose& pose, const RenderState_CPtr& renderState);

  //#################### PRIVATE MEMBER FUNCTIONS ####################
private:
  /**
   * \brief TODO
   */
  ITMUCharImage_CPtr make_change_mask(const ITMFloatImage_CPtr& depthInput, const ORUtils::SE3Pose& pose, const RenderState_CPtr& renderState) const;

  /**
   * \brief TODO
   */
  ITMUCharImage_CPtr make_touch_mask(const ITMFloatImage_CPtr& depthInput, const ORUtils::SE3Pose& pose, const RenderState_CPtr& renderState) const;
};

}

#endif
