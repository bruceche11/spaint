/**
 * spaint: SpaintDecisionFunctionGenerator.cpp
 * Copyright (c) Torr Vision Group, University of Oxford, 2015. All rights reserved.
 */

#include "randomforest/SpaintDecisionFunctionGenerator.h"

#include <rafl/decisionfunctions/FeatureThresholdingDecisionFunctionGenerator.h>
#include <rafl/decisionfunctions/PairwiseOpAndThresholdDecisionFunctionGenerator.h>
using namespace rafl;

#define USE_HEIGHT_THRESHOLDING 0

namespace spaint {

//#################### CONSTRUCTORS ####################

SpaintDecisionFunctionGenerator::SpaintDecisionFunctionGenerator(size_t patchSize)
: m_patchSize(patchSize)
{
  int vopFeatureCount = static_cast<int>(patchSize * patchSize * 3);
  std::pair<int,int> vopFeatureIndexRange(0, vopFeatureCount - 1);
  std::pair<int,int> normalFeatureIndexRange(vopFeatureCount, vopFeatureCount + 2);

#ifndef USE_LOW_POWER_MODE
  this->add_generator(DecisionFunctionGenerator_CPtr(new FeatureThresholdingDecisionFunctionGenerator<Label>(vopFeatureIndexRange)));
  this->add_generator(DecisionFunctionGenerator_CPtr(new PairwiseOpAndThresholdDecisionFunctionGenerator<Label>(vopFeatureIndexRange)));
#endif

  this->add_generator(DecisionFunctionGenerator_CPtr(new FeatureThresholdingDecisionFunctionGenerator<Label>(normalFeatureIndexRange)));
  this->add_generator(DecisionFunctionGenerator_CPtr(new PairwiseOpAndThresholdDecisionFunctionGenerator<Label>(normalFeatureIndexRange)));

#if USE_HEIGHT_THRESHOLDING
  this->add_generator(DecisionFunctionGenerator_CPtr(new FeatureThresholdingDecisionFunctionGenerator<Label>(std::make_pair(vopFeatureCount + 3, vopFeatureCount + 3))));
#endif
}

//#################### PUBLIC STATIC MEMBER FUNCTIONS ####################

std::string SpaintDecisionFunctionGenerator::get_static_type()
{
  return "Spaint";
}

SpaintDecisionFunctionGenerator::DecisionFunctionGenerator_Ptr SpaintDecisionFunctionGenerator::maker(const std::string& params)
{
  size_t patchSize = boost::lexical_cast<size_t>(params);
  return DecisionFunctionGenerator_Ptr(new SpaintDecisionFunctionGenerator(patchSize));
}

//#################### PUBLIC MEMBER FUNCTIONS ####################

std::string SpaintDecisionFunctionGenerator::get_params() const
{
  return boost::lexical_cast<std::string>(m_patchSize);
}

std::string SpaintDecisionFunctionGenerator::get_type() const
{
  return get_static_type();
}

}
