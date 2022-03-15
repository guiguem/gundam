//
// Created by Nadrino on 11/06/2021.
//

#include "Propagator.h"

#ifdef GUNDAM_USING_CUDA
#include "GPUInterpCachedWeights.h"
#endif

#include "FitParameterSet.h"
#include "Dial.h"
#include "SplineDial.h"
#include "GraphDial.h"
#include "NormalizationDial.h"
#include "JsonUtils.h"
#include "GlobalVariables.h"
#include <AnaTreeMC.hh>

#include "GenericToolbox.h"
#include "GenericToolbox.Root.h"

#include <vector>
#include <set>

LoggerInit([](){
  Logger::setUserHeaderStr("[Propagator]");
})

Propagator::Propagator() { this->reset(); }
Propagator::~Propagator() { this->reset(); }

void Propagator::reset() {
  _isInitialized_ = false;
  _parameterSetsList_.clear();
  _saveDir_ = nullptr;

  std::vector<std::string> jobNameRemoveList;
  for( const auto& jobName : GlobalVariables::getParallelWorker().getJobNameList() ){
    if(jobName == "Propagator::fillEventDialCaches"
    or jobName == "Propagator::reweightSampleEvents"
    or jobName == "Propagator::updateDialResponses"
    or jobName == "Propagator::refillSampleHistograms"
    or jobName == "Propagator::applyResponseFunctions"
      ){
      jobNameRemoveList.emplace_back(jobName);
    }
  }
  for( const auto& jobName : jobNameRemoveList ){
    GlobalVariables::getParallelWorker().removeJob(jobName);
  }

  _responseFunctionsSamplesMcHistogram_.clear();
  _nominalSamplesMcHistogram_.clear();
}

void Propagator::setShowTimeStats(bool showTimeStats) {
  _showTimeStats_ = showTimeStats;
}
void Propagator::setSaveDir(TDirectory *saveDir) {
  _saveDir_ = saveDir;
}
void Propagator::setConfig(const json &config) {
  _config_ = config;
  while( _config_.is_string() ){
    LogWarning << "Forwarding " << __CLASS_NAME__ << " config: \"" << _config_.get<std::string>() << "\"" << std::endl;
    _config_ = JsonUtils::readConfigFile(_config_.get<std::string>());
  }
}

void Propagator::initialize() {
  LogWarning << __METHOD_NAME__ << std::endl;

  LogInfo << "Loading Parameters..." << std::endl;
  auto parameterSetListConfig = JsonUtils::fetchValue(_config_, "parameterSetListConfig", nlohmann::json());
  if( parameterSetListConfig.is_string() ) parameterSetListConfig = JsonUtils::readConfigFile(parameterSetListConfig.get<std::string>());
  int nPars = 0;
  _parameterSetsList_.reserve(parameterSetListConfig.size()); // make sure the objects aren't moved in RAM ( since FitParameter* will be used )
  for( const auto& parameterSetConfig : parameterSetListConfig ){
    _parameterSetsList_.emplace_back();
    _parameterSetsList_.back().setConfig(parameterSetConfig);
    _parameterSetsList_.back().setSaveDir(GenericToolbox::mkdirTFile(_saveDir_, "ParameterSets"));
    _parameterSetsList_.back().initialize();
    nPars += _parameterSetsList_.back().getNbParameters();
    LogInfo << _parameterSetsList_.back().getSummary() << std::endl;
  }

  _globalCovarianceMatrix_ = std::shared_ptr<TMatrixD>( new TMatrixD(nPars, nPars) );
  int iParOffset = 0;
  for( const auto& parSet : _parameterSetsList_ ){
    if( not parSet.isEnabled() ) continue;
    if(parSet.getPriorCovarianceMatrix() != nullptr ){
      for(int iCov = 0 ; iCov < parSet.getPriorCovarianceMatrix()->GetNrows() ; iCov++ ){
        for(int jCov = 0 ; jCov < parSet.getPriorCovarianceMatrix()->GetNcols() ; jCov++ ){
          (*_globalCovarianceMatrix_)[iParOffset+iCov][iParOffset+jCov] = (*parSet.getPriorCovarianceMatrix())[iCov][jCov];
        }
      }
      iParOffset += parSet.getPriorCovarianceMatrix()->GetNrows();
    }
  }
  if( _saveDir_ != nullptr ){
    _saveDir_->cd();
    _globalCovarianceMatrix_->Write("globalCovarianceMatrix_TMatrixD");
  }

  LogInfo << "Initializing FitSampleSet" << std::endl;
  auto fitSampleSetConfig = JsonUtils::fetchValue(_config_, "fitSampleSetConfig", nlohmann::json());
  _fitSampleSet_.setConfig(fitSampleSetConfig);
  _fitSampleSet_.initialize();

  LogInfo << "Initializing the PlotGenerator" << std::endl;
  auto plotGeneratorConfig = JsonUtils::fetchValue(_config_, "plotGeneratorConfig", nlohmann::json());
  if( plotGeneratorConfig.is_string() ) parameterSetListConfig = JsonUtils::readConfigFile(plotGeneratorConfig.get<std::string>());
  _plotGenerator_.setConfig(plotGeneratorConfig);
  _plotGenerator_.initialize();

  LogInfo << "Initializing input datasets..." << std::endl;
  auto dataSetListConfig = JsonUtils::getForwardedConfig(_config_, "dataSetList");
  if( dataSetListConfig.empty() ){
    // Old config files
    dataSetListConfig = JsonUtils::getForwardedConfig(_fitSampleSet_.getConfig(), "dataSetList");;
    LogAlert << "DEPRECATED CONFIG OPTION: " << "dataSetList should now be located in the Propagator config." << std::endl;
  }
  LogThrowIf(dataSetListConfig.empty(), "No dataSet specified." << std::endl);
  int iDataSet{0};
  for( const auto& dataSetConfig : dataSetListConfig ){
    _dataSetList_.emplace_back();
    _dataSetList_.back().setConfig(dataSetConfig);
    _dataSetList_.back().setDataSetIndex(iDataSet++);
    _dataSetList_.back().initialize();
  }

  LogInfo << "Loading datasets..." << std::endl;
  for( auto& dataSet : _dataSetList_ ){
    dataSet.fetchRequestedLeaves(&_plotGenerator_);
    dataSet.load(&_fitSampleSet_, &_parameterSetsList_);
  } // dataSets

  _fitSampleSet_.loadAsimovData(); // Copies MC events in data container for both Asimov and FakeData event types

//  if( GlobalVariables::isEnableDevMode() ){
//    LogInfo << "Loading dials stack..." << std::endl;
//    fillDialsStack();
//  }

  LogInfo << "Initializing threads..." << std::endl;
  initializeThreads();

  LogInfo << "Propagating prior parameters on events..." << std::endl;
  reweightSampleEvents();

  LogInfo << "Set the current MC prior weights as nominal weight..." << std::endl;
  for( auto& sample : _fitSampleSet_.getFitSampleList() ){
    for( auto& event : sample.getMcContainer().eventList ){
      event.setNominalWeight(event.getEventWeight());
    }
  }

  if(   _fitSampleSet_.getDataEventType() == DataEventType::Asimov
    or _fitSampleSet_.getDataEventType() == DataEventType::FakeData
  ){
    LogInfo << "Propagating prior weights on data Asimov/FakeData events..." << std::endl;

    if( JsonUtils::fetchValue<json>(_config_, "throwAsimovFitParameters", false) ){
      LogWarning << "Throwing fit parameters for Asimov data..." << std::endl;
      for( auto& parSet : _parameterSetsList_ ){
        if( not parSet.isEnabled() ) continue;
        parSet.throwFitParameters();
      }
      reweightSampleEvents();
    }

    double weightBuffer;
    for( auto& sample : _fitSampleSet_.getFitSampleList() ){
      sample.getDataContainer().histScale = sample.getMcContainer().histScale;

      auto* mcEventList = &sample.getMcContainer().eventList;
      auto* dataEventList = &sample.getDataContainer().eventList;

      int nEvents = int(mcEventList->size());
      for( int iEvent = 0 ; iEvent < nEvents ; iEvent++ ){
        // Since no reweight is applied on data samples, the nominal weight should be the default one
        weightBuffer = (*mcEventList)[iEvent].getEventWeight();
        if( _fitSampleSet_.getDataEventType() == DataEventType::FakeData ){
          weightBuffer *= (*mcEventList)[iEvent].getFakeDataWeight();
        }

        (*dataEventList)[iEvent].setTreeWeight(weightBuffer);
        (*dataEventList)[iEvent].resetEventWeight();              // treeWeight -> eventWeight
        (*dataEventList)[iEvent].setNominalWeight(weightBuffer);  // also on nominal (but irrelevant for data-like samples)
      }
    }

    // Make sure MC event are back at their nominal value:
    if( JsonUtils::fetchValue<json>(_config_, "throwAsimovFitParameters", false) ){
      for( auto& parSet : _parameterSetsList_ ){
        if( not parSet.isEnabled() ) continue;
        parSet.moveFitParametersToPrior();
      }
      reweightSampleEvents();
    }

  }

  LogWarning << "Sample breakdown:" << std::endl;
  for( auto& sample : _fitSampleSet_.getFitSampleList() ){
    LogInfo << "Sum of event weights in \"" << sample.getName() << "\":" << std::endl
            << "-> mc: " << sample.getMcContainer().getSumWeights()
            << " / data: " << sample.getDataContainer().getSumWeights() << std::endl;
  }

  _plotGenerator_.setFitSampleSetPtr(&_fitSampleSet_);
  _plotGenerator_.defineHistogramHolders();

  LogInfo << "Filling up sample bin caches..." << std::endl;
  _fitSampleSet_.updateSampleBinEventList();

  LogInfo << "Filling up sample histograms..." << std::endl;
  _fitSampleSet_.updateSampleHistograms();

  // Now the data won't be refilled each time
  for( auto& sample : _fitSampleSet_.getFitSampleList() ){
    sample.getDataContainer().isLocked = true;
  }

  _useResponseFunctions_ = JsonUtils::fetchValue<json>(_config_, "DEV_useResponseFunctions", false);
  if( _useResponseFunctions_ ){ this->makeResponseFunctions(); }

  if( JsonUtils::fetchValue<json>(_config_, "throwAsimovFitParameters", false) ){
    for( auto& parSet : _parameterSetsList_ ){
      for( auto& par : parSet.getParameterList() ){
        par.setParameterValue( par.getPriorValue() );
      }
    }
  }

#ifdef GUNDAM_USING_CUDA
  // After all of the data has been loaded.  Specifically, this must be after
  // the MC has been copied for the Asimov fit, or the "data" use the MC
  // reweighting cache.
  buildGPUCaches();
#endif

  _treeWriter_.setFitSampleSetPtr(&_fitSampleSet_);
  _treeWriter_.setParSetListPtr(&_parameterSetsList_);

  _isInitialized_ = true;
}

bool Propagator::isUseResponseFunctions() const {
  return _useResponseFunctions_;
}
FitSampleSet &Propagator::getFitSampleSet() {
  return _fitSampleSet_;
}
std::vector<FitParameterSet> &Propagator::getParameterSetsList() {
  return _parameterSetsList_;
}
const std::vector<FitParameterSet> &Propagator::getParameterSetsList() const {
  return _parameterSetsList_;
}
PlotGenerator &Propagator::getPlotGenerator() {
  return _plotGenerator_;
}
const json &Propagator::getConfig() const {
  return _config_;
}


void Propagator::propagateParametersOnSamples(){

  // Only real parameters are propagated on the specta -> need to convert the eigen to original
  for( auto& parSet : _parameterSetsList_ ){
    if( parSet.isUseEigenDecompInFit() ) parSet.propagateEigenToOriginal();
  }

  if(not _useResponseFunctions_ or not _isRfPropagationEnabled_ ){
//    if(GlobalVariables::isEnableDevMode()) updateDialResponses();
    reweightSampleEvents();
    refillSampleHistograms();
  }
  else{
    applyResponseFunctions();
  }

}
void Propagator::updateDialResponses(){
  GenericToolbox::getElapsedTimeSinceLastCallInMicroSeconds(__METHOD_NAME__);
  GlobalVariables::getParallelWorker().runJob("Propagator::updateDialResponses");
  dialUpdate.counts++; dialUpdate.cumulated += GenericToolbox::getElapsedTimeSinceLastCallInMicroSeconds(__METHOD_NAME__);
}
void Propagator::reweightSampleEvents() {
  GenericToolbox::getElapsedTimeSinceLastCallInMicroSeconds(__METHOD_NAME__);
  bool usedGPU = false;
#ifdef GUNDAM_USING_CUDA
  usedGPU = fillGPUCaches();
#endif
  if (!usedGPU) {
      GlobalVariables::getParallelWorker().runJob(
          "Propagator::reweightSampleEvents");
  }
  weightProp.counts++;
  weightProp.cumulated += GenericToolbox::getElapsedTimeSinceLastCallInMicroSeconds(__METHOD_NAME__);
}

void Propagator::refillSampleHistograms(){
  GenericToolbox::getElapsedTimeSinceLastCallInMicroSeconds(__METHOD_NAME__);
  GlobalVariables::getParallelWorker().runJob("Propagator::refillSampleHistograms");
  fillProp.counts++; fillProp.cumulated += GenericToolbox::getElapsedTimeSinceLastCallInMicroSeconds(__METHOD_NAME__);
}

void Propagator::applyResponseFunctions(){
  GenericToolbox::getElapsedTimeSinceLastCallInMicroSeconds(__METHOD_NAME__);
  GlobalVariables::getParallelWorker().runJob("Propagator::applyResponseFunctions");
  applyRf.counts++; applyRf.cumulated += GenericToolbox::getElapsedTimeSinceLastCallInMicroSeconds(__METHOD_NAME__);
}

void Propagator::preventRfPropagation(){
  if(_isRfPropagationEnabled_){
//    LogInfo << "Parameters propagation using Response Function is now disabled." << std::endl;
    _isRfPropagationEnabled_ = false;
  }
}
void Propagator::allowRfPropagation(){
  if(not _isRfPropagationEnabled_){
//    LogWarning << "Parameters propagation using Response Function is now ENABLED." << std::endl;
    _isRfPropagationEnabled_ = true;
  }
}

void Propagator::fillDialsStack(){
  for( auto& parSet : _parameterSetsList_ ){
    if( not parSet.isUseEigenDecompInFit() ){
      for( auto& par : parSet.getParameterList() ){
        for( auto& dialSet : par.getDialSetList() ){
          if(dialSet.getGlobalDialType() == DialType::Normalization){ continue; } // no cache needed
          for( auto& dial : dialSet.getDialList() ){
            if(dial->isReferenced()) _dialsStack_.emplace_back(dial.get());
          }
        }
      }
    }
  }
}


// Protected
void Propagator::initializeThreads() {

  std::function<void(int)> reweightSampleEventsFct = [this](int iThread){
    this->reweightSampleEvents(iThread);
  };
  GlobalVariables::getParallelWorker().addJob("Propagator::reweightSampleEvents", reweightSampleEventsFct);

  std::function<void(int)> updateDialResponsesFct = [this](int iThread){
    this->updateDialResponses(iThread);
  };
  GlobalVariables::getParallelWorker().addJob("Propagator::updateDialResponses", updateDialResponsesFct);


  std::function<void(int)> refillSampleHistogramsFct = [this](int iThread){
    for( auto& sample : _fitSampleSet_.getFitSampleList() ){
      sample.getMcContainer().refillHistogram(iThread);
      sample.getDataContainer().refillHistogram(iThread);
    }
  };
  std::function<void()> refillSampleHistogramsPostParallelFct = [this](){
    for( auto& sample : _fitSampleSet_.getFitSampleList() ){
      sample.getMcContainer().rescaleHistogram();
      sample.getDataContainer().rescaleHistogram();
    }
  };
  GlobalVariables::getParallelWorker().addJob("Propagator::refillSampleHistograms", refillSampleHistogramsFct);
  GlobalVariables::getParallelWorker().setPostParallelJob("Propagator::refillSampleHistograms", refillSampleHistogramsPostParallelFct);

  std::function<void(int)> applyResponseFunctionsFct = [this](int iThread){
    this->applyResponseFunctions(iThread);
  };
  GlobalVariables::getParallelWorker().addJob("Propagator::applyResponseFunctions", applyResponseFunctionsFct);

  GlobalVariables::getParallelWorker().setCpuTimeSaverIsEnabled(false);

}

void Propagator::makeResponseFunctions(){
  LogWarning << __METHOD_NAME__ << std::endl;

  this->preventRfPropagation(); // make sure, not yet setup

  for( auto& parSet : _parameterSetsList_ ){
    for( auto& par : parSet.getParameterList() ){
      par.setParameterValue(par.getPriorValue());
    }
  }
  this->propagateParametersOnSamples();

  for( auto& sample : _fitSampleSet_.getFitSampleList() ){
    _nominalSamplesMcHistogram_[&sample] = std::shared_ptr<TH1D>((TH1D*) sample.getMcContainer().histogram->Clone());
  }

  for( auto& parSet : _parameterSetsList_ ){
    for( auto& par : parSet.getParameterList() ){
      LogInfo << "Make RF for " << parSet.getName() << "/" << par.getTitle() << std::endl;
      par.setParameterValue(par.getPriorValue() + par.getStdDevValue());

      this->propagateParametersOnSamples();

      for( auto& sample : _fitSampleSet_.getFitSampleList() ){
        _responseFunctionsSamplesMcHistogram_[&sample].emplace_back(std::shared_ptr<TH1D>((TH1D*) sample.getMcContainer().histogram->Clone()) );
        GenericToolbox::transformBinContent(_responseFunctionsSamplesMcHistogram_[&sample].back().get(), [&](TH1D* h_, int b_){
          h_->SetBinContent(
            b_,
            (h_->GetBinContent(b_)/_nominalSamplesMcHistogram_[&sample]->GetBinContent(b_))-1);
          h_->SetBinError(b_,0);
        });
      }

      par.setParameterValue(par.getPriorValue());
    }
  }
  this->propagateParametersOnSamples(); // back to nominal

  // WRITE
  if( _saveDir_ != nullptr ){
    auto* rfDir = GenericToolbox::mkdirTFile(_saveDir_, "RF");
    for( auto& sample : _fitSampleSet_.getFitSampleList() ){
      GenericToolbox::mkdirTFile(rfDir, "nominal")->cd();
      _nominalSamplesMcHistogram_[&sample]->Write(Form("nominal_%s", sample.getName().c_str()));

      int iPar = -1;
      auto* devDir = GenericToolbox::mkdirTFile(rfDir, "deviation");
      for( auto& parSet : _parameterSetsList_ ){
        auto* parSetDir = GenericToolbox::mkdirTFile(devDir, parSet.getName());
        for( auto& par : parSet.getParameterList() ){
          iPar++;
          GenericToolbox::mkdirTFile(parSetDir, par.getTitle())->cd();
          _responseFunctionsSamplesMcHistogram_[&sample].at(iPar)->Write(Form("dev_%s", sample.getName().c_str()));
        }
      }
    }
    _saveDir_->cd();
  }

  LogInfo << "RF built" << std::endl;
}

void Propagator::updateDialResponses(int iThread_){
  int nThreads = GlobalVariables::getNbThreads();
  if(iThread_ == -1){
    // force single thread
    nThreads = 1;
    iThread_ = 0;
  }
  int iDial{0};
  int nDials(int(_dialsStack_.size()));

  while( iDial < nDials ){
    _dialsStack_[iDial]->evalResponse();
    iDial += nThreads;
  }

}

#ifdef GUNDAM_USING_CUDA
bool Propagator::buildGPUCaches() {
    LogInfo << "Build the GPU Caches" << std::endl;

    int events = 0;
    int splines = 0;
    int splinePoints = 0;
    int uSplines = 0;
    int uSplinePoints = 0;
    int graphs = 0;
    int graphPoints = 0;
    int norms = 0;
    std::set<const FitParameter*> usedParameters;
    for(const FitSample& sample : getFitSampleSet().getFitSampleList() ){
        LogInfo << "Sample " << sample.getName()
                << " with " << sample.getMcContainer().eventList.size()
                << " events" << std::endl;
        for (const PhysicsEvent& event
                 : sample.getMcContainer().eventList) {
            ++events;
            if (event.getSampleBinIndex() < 0) {
                throw std::runtime_error("Caching event that isn't used");
            }
            for (const Dial* dial
                     : event.getRawDialPtrList()) {
                FitParameter* fp = static_cast<FitParameter*>(
                    dial->getAssociatedParameterReference());
                usedParameters.insert(fp);
                const SplineDial* sDial
                    = dynamic_cast<const SplineDial*>(dial);
                if (sDial) {
                    const TSpline3* s = sDial->getSplinePtr();
                    if (!s) throw std::runtime_error("Null spline pointer");
                    if (s->GetDelta() > 0) {
                        ++uSplines;
                        uSplinePoints += 2*s->GetNp();
                    }
                    ++splines;
                    splinePoints += s->GetNp();
                }
                const GraphDial* gDial
                    = dynamic_cast<const GraphDial*>(dial);
                if (gDial) {
                    ++graphs;
                }
                const NormalizationDial* nDial
                    = dynamic_cast<const NormalizationDial*>(dial);
                if (nDial) {
                    ++norms;
                }
            }
        }
    }

    int parameters = usedParameters.size();

    LogInfo << "GPU Cache for " << events << " events --"
            << " Par: " << parameters
            << " Uniform splines: " << splines
            << " (" << 1.0*splines/events << ")"
            << " General Splines: " << uSplines
            << " (" << 1.0*uSplines/events << ")"
            << " G " << graphs << " (" << 1.0*graphs/events << ")"
            << " N: " << norms <<" ("<< 1.0*norms/events <<")"
            << std::endl;
    if (splines > 0) {
        LogInfo << "Uniform spline cache for "
                << splinePoints << " control points --"
                << " (" << 1.0*splinePoints/splines << " points per spline)"
                << std::endl;
    }
    if (uSplines > 0) {
        LogInfo << "General spline cache for "
                << uSplinePoints << " control points --"
                << " (" << 1.0*uSplinePoints/uSplines << " points per spline)"
                << std::endl;
    }
    if (graphs > 0) {
        LogInfo << "Graph cache for " << graphPoints << " control points --"
                << " (" << 1.0*graphPoints/graphs << " points per graph)"
                << std::endl;
    }

    // Try to allocate the GPU
    if (!GPUInterp::CachedWeights::Get()
        && GlobalVariables::getEnableEventWeightCache()) {
        LogInfo << "Creating GPU spline cache" << std::endl;
        GPUInterp::CachedWeights::Create(
            events,parameters,norms,splines,splinePoints);
    }

    // In case the GPU didn't get allocated.
    if (!GPUInterp::CachedWeights::Get()) {
        LogInfo << "No CachedWeights for GPU"
                << std::endl;
        return false;
    }

    int usedResults = 0; // Number of cached results that have been used up.
    for(FitSample& sample : getFitSampleSet().getFitSampleList() ) {
        LogInfo << "Fill GPU cache for " << sample.getName()
                << " with " << sample.getMcContainer().eventList.size()
                << " events" << std::endl;
        for (PhysicsEvent& event
                 : sample.getMcContainer().eventList) {
            // The reduce index to save the result for this event.
            int resultIndex = usedResults++;
            event.setResultIndex(resultIndex);
            event.setResultPointer(GPUInterp::CachedWeights::Get()
                                   ->GetResultPointer(resultIndex));
            GPUInterp::CachedWeights::Get()
                ->SetInitialValue(resultIndex, event.getTreeWeight());
            for (Dial* dial
                     : event.getRawDialPtrList()) {
                if (!dial->isReferenced()) continue;
                FitParameter* fp = static_cast<FitParameter*>(
                    dial->getAssociatedParameterReference());
                std::map<const FitParameter*,int>::iterator parMapIt
                    = _gpuParameterIndex_.find(fp);
                if (parMapIt == _gpuParameterIndex_.end()) {
                    _gpuParameterIndex_[fp] = _gpuParameterIndex_.size();
                }
                int parIndex = _gpuParameterIndex_[fp];
                int dialUsed = 0;
                if(dial->getDialType() == DialType::Normalization) {
                    ++dialUsed;
                    GPUInterp::CachedWeights::Get()
                        ->ReserveNorm(resultIndex,parIndex);
                }
                SplineDial* sDial = dynamic_cast<SplineDial*>(dial);
                if (sDial) {
                    ++dialUsed;
                    const TSpline3* s = sDial->getSplinePtr();
                    if (!s) throw std::runtime_error("Null spline pointer");
                    double xMin = s->GetXmin();
                    double xMax = s->GetXmax();
                    int NP = s->GetNp();
                    int spline = GPUInterp::CachedWeights::Get()
                        ->ReserveSpline(resultIndex,parIndex,
                                        xMin,xMax,
                                        NP);

                    // BUG!!!! SUPER MAJOR CHEAT:  This is forcing all the
                    // splines to have uniform control points.
                    for (int i=0; i<NP; ++i) {
                        double x = xMin + i*(xMax-xMin)/(NP-1);
                        double y = s->Eval(x);
                        GPUInterp::CachedWeights::Get()
                            ->SetSplineKnot(spline,i, y);
                    }

                    if (sDial->getUseMirrorDial()) {
                        double xLow = sDial->getMirrorLowEdge();
                        double xHigh = xLow + sDial->getMirrorRange();
                        GPUInterp::CachedWeights::Get()
                            ->SetLowerMirror(parIndex,xLow);
                        GPUInterp::CachedWeights::Get()
                            ->SetUpperMirror(parIndex,xHigh);
                    }
                }
                if (!dialUsed) throw std::runtime_error("Unused dial");
            }
        }
    }

    if (usedResults == GPUInterp::CachedWeights::Get()->GetResultCount()) {
        return true;
    }

    LogInfo << "GPU Used Results:     " << usedResults << std::endl;
    LogInfo << "GPU Expected Results: " <<
        GPUInterp::CachedWeights::Get()->GetResultCount() << std::endl;
    throw std::runtime_error("Probable problem putting parameters in cache");
}

bool Propagator::fillGPUCaches() {
    GPUInterp::CachedWeights* gpu = GPUInterp::CachedWeights::Get();
    if (!gpu) return false;
    for (auto& par : _gpuParameterIndex_ ) {
        gpu->SetParameter(par.second, par.first->getParameterValue());
    }
    gpu->UpdateResults();
    return true;
}
#endif


void Propagator::reweightSampleEvents(int iThread_) {
  int nThreads = GlobalVariables::getNbThreads();
  if(iThread_ == -1){
    // force single thread
    nThreads = 1;
    iThread_ = 0;
  }

  //! Warning: everything you modify here, may significantly slow down the fitter

  // This loop is slightly faster that the next one (~1% faster)
  // Memory needed: 2*32bits(int) + 64bits(ptr)
  // 3 write per sample
  // 1 write per event
  int iEvent;
  int nEvents;
  std::vector<PhysicsEvent>* evList{nullptr};
  for( auto& sample : _fitSampleSet_.getFitSampleList() ){
    evList = &sample.getMcContainer().eventList;
    iEvent = iThread_;
    nEvents = int(evList->size());
    while( iEvent < nEvents ){
      (*evList)[iEvent].reweightUsingDialCache();
      iEvent += nThreads;
    }
  }

//  // Slower loop
//  // Memory: 2*64bits
//  // per sample: 1 read, 2 writes (each requires to fetch the event array multiple times)
//  // 1 write per event
//  PhysicsEvent* evPtr{nullptr};
//  PhysicsEvent* evLastPtr{nullptr};
//  for( auto& sample : _fitSampleSet_.getFitSampleList() ){
//    if( sample.getMcContainer().eventList.empty() ) continue;
//    evPtr = &sample.getMcContainer().eventList[iThread_];
//    evLastPtr = &sample.getMcContainer().eventList.back();
//
//    while( evPtr <= evLastPtr ){
//      evPtr->reweightUsingDialCache();
//      evPtr += nThreads;
//    }
//  }

}
void Propagator::applyResponseFunctions(int iThread_){

  TH1D* histBuffer{nullptr};
  TH1D* nominalHistBuffer{nullptr};
  TH1D* rfHistBuffer{nullptr};
  for( auto& sample : _fitSampleSet_.getFitSampleList() ){
    histBuffer = sample.getMcContainer().histogram.get();
    nominalHistBuffer = _nominalSamplesMcHistogram_[&sample].get();
    for( int iBin = 1 ; iBin <= histBuffer->GetNbinsX() ; iBin++ ){
      if( iBin % GlobalVariables::getNbThreads() != iThread_ ) continue;
      histBuffer->SetBinContent(iBin, nominalHistBuffer->GetBinContent(iBin));
    }
  }

  int iPar = -1;
  for( auto& parSet : _parameterSetsList_ ){
    for( auto& par : parSet.getParameterList() ){
      iPar++;
      double xSigmaPar = par.getDistanceFromNominal();
      if( xSigmaPar == 0 ) continue;

      for( auto& sample : _fitSampleSet_.getFitSampleList() ){
        histBuffer = sample.getMcContainer().histogram.get();
        nominalHistBuffer = _nominalSamplesMcHistogram_[&sample].get();
        rfHistBuffer = _responseFunctionsSamplesMcHistogram_[&sample][iPar].get();

        for( int iBin = 1 ; iBin <= histBuffer->GetNbinsX() ; iBin++ ){
          if( iBin % GlobalVariables::getNbThreads() != iThread_ ) continue;
          histBuffer->SetBinContent(
            iBin,
            histBuffer->GetBinContent(iBin) * ( 1 + xSigmaPar * rfHistBuffer->GetBinContent(iBin) )
          );
        }
      }
    }
  }

  for( auto& sample : _fitSampleSet_.getFitSampleList() ){
    histBuffer = sample.getMcContainer().histogram.get();
    nominalHistBuffer = _nominalSamplesMcHistogram_[&sample].get();
    for( int iBin = 1 ; iBin <= histBuffer->GetNbinsX() ; iBin++ ){
      if( iBin % GlobalVariables::getNbThreads() != iThread_ ) continue;
      histBuffer->SetBinError(iBin, TMath::Sqrt(histBuffer->GetBinContent(iBin)));
//      if( iThread_ == 0 ){
//        LogTrace << GET_VAR_NAME_VALUE(iBin)
//        << " / " << GET_VAR_NAME_VALUE(histBuffer->GetBinContent(iBin))
//        << " / " << GET_VAR_NAME_VALUE(nominalHistBuffer->GetBinContent(iBin))
//        << std::endl;
//      }
    }
  }

}

const EventTreeWriter &Propagator::getTreeWriter() const {
  return _treeWriter_;
}
