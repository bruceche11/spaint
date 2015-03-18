/**
 * spaint: ImageProcessing_CPU.h
 */

#ifndef H_SPAINT_IMAGEPROCESSING_CPU
#define H_SPAINT_IMAGEPROCESSING_CPU

#include "../interface/ImageProcessing.h"

namespace spaint {

/**
 * \brief An instance of this class may be used to apply image processing algorithms to images on the CPU.
 */
class ImageProcessing_CPU : public ImageProcessing
{
  //#################### PUBLIC MEMBER FUNCTIONS ####################
public:
  /** Override. */
  virtual void absolute_difference_calculator(ITMFloatImage *outputImage, ITMFloatImage *firstInputImage, ITMFloatImage *secondInputImage) const;

  /** Override. */
  virtual void absolute_difference_calculator(af::array *outputImage, ITMFloatImage *firstInputImage, ITMFloatImage *secondInputImage) const;
};

}

#endif
