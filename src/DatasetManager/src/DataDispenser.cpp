//
// Created by Adrien BLANCHET on 14/05/2022.
//


#include "DataDispenser.h"

#include "EventVarTransform.h"
#include "GundamGlobals.h"
#include "DatasetLoader.h"
#include "GenericToolbox.Json.h"
#include "ConfigUtils.h"

#include "DialCollection.h"
#include "DialTypes.h"
#include "DialBaseFactory.h"

#include "GenericToolbox.Root.h"
#include "GenericToolbox.Root.LeafCollection.h"
#include "GenericToolbox.VariablesMonitor.h"
#include "Logger.h"

#include "TTreeFormulaManager.h"
#include "TChain.h"
#include "TChainElement.h"
#include "THn.h"

#include <string>
#include <vector>
#include <sstream>

LoggerInit([]{
  Logger::setUserHeaderStr("[DataDispenser]");
});


void DataDispenser::readConfigImpl(){
  LogThrowIf( _config_.empty(), "Config is not set." );

  _parameters_.name = GenericToolbox::Json::fetchValue<std::string>(_config_, "name", _parameters_.name);

  if( GenericToolbox::Json::doKeyExist( _config_, "fromHistContent" ) ) {
    LogWarning << "Dataset \"" << _parameters_.name << "\" will be defined with histogram data." << std::endl;

    _parameters_.fromHistContent = GenericToolbox::Json::fetchValue<nlohmann::json>( _config_, "fromHistContent" );
    ConfigUtils::forwardConfig( _parameters_.fromHistContent );
    return;
  }

  _parameters_.treePath = GenericToolbox::Json::fetchValue<std::string>(_config_, "tree", _parameters_.treePath);
  _parameters_.filePathList = GenericToolbox::Json::fetchValue<std::vector<std::string>>(_config_, "filePathList", _parameters_.filePathList);
  _parameters_.additionalVarsStorage = GenericToolbox::Json::fetchValue(_config_, {{"additionalLeavesStorage"}, {"additionalVarsStorage"}}, _parameters_.additionalVarsStorage);
  _parameters_.dummyVariablesList = GenericToolbox::Json::fetchValue(_config_, "dummyVariablesList", _parameters_.dummyVariablesList);
  _parameters_.useMcContainer = GenericToolbox::Json::fetchValue(_config_, "useMcContainer", _parameters_.useMcContainer);

  _parameters_.dialIndexFormula = GenericToolbox::Json::fetchValue(_config_, "dialIndexFormula", _parameters_.dialIndexFormula);
  _parameters_.selectionCutFormulaStr = GenericToolbox::Json::buildFormula(_config_, "selectionCutFormula", "&&", _parameters_.selectionCutFormulaStr);
  _parameters_.nominalWeightFormulaStr = GenericToolbox::Json::buildFormula(_config_, "nominalWeightFormula", "*", _parameters_.nominalWeightFormulaStr);

  _parameters_.overrideLeafDict.clear();
  for( auto& entry : GenericToolbox::Json::fetchValue(_config_, "overrideLeafDict", nlohmann::json()) ){
    _parameters_.overrideLeafDict[entry["eventVar"]] = entry["leafVar"];
  }
}
void DataDispenser::initializeImpl(){
  // Nothing else to do other than read config?
  LogWarning << "Initialized data dispenser: " << getTitle() << std::endl;
}

void DataDispenser::setSampleSetPtrToLoad(SampleSet *sampleSetPtrToLoad) {
  _sampleSetPtrToLoad_ = sampleSetPtrToLoad;
}
void DataDispenser::setParSetPtrToLoad(std::vector<ParameterSet> *parSetListPtrToLoad_) {
  _parSetListPtrToLoad_ = parSetListPtrToLoad_;
}
void DataDispenser::setDialCollectionListPtr(std::vector<DialCollection> *dialCollectionListPtr) {
  _dialCollectionListPtr_ = dialCollectionListPtr;
}
void DataDispenser::setPlotGenPtr(PlotGenerator *plotGenPtr) {
  _plotGenPtr_ = plotGenPtr;
}
void DataDispenser::setEventDialCache(EventDialCache* eventDialCache_) {
  _eventDialCacheRef_ = eventDialCache_;
}

void DataDispenser::load(){
  LogWarning << "Loading dataset: " << getTitle() << std::endl;
  LogThrowIf(not this->isInitialized(), "Can't load while not initialized.");
  LogThrowIf(_sampleSetPtrToLoad_==nullptr, "SampleSet not specified.");

  if(GundamGlobals::getVerboseLevel() >= MORE_PRINTOUT ){
    LogDebug << "Configuration: " << _parameters_.getSummary() << std::endl;
  }

  _cache_.clear();

  this->buildSampleToFillList();

  if( _cache_.samplesToFillList.empty() ){
    LogAlert << "No samples were selected for dataset: " << getTitle() << std::endl;
    return;
  }

  if( not _parameters_.fromHistContent.empty() ){
    this->loadFromHistContent();
    return;
  }

  LogInfo << "Data will be extracted from: " << GenericToolbox::parseVectorAsString(_parameters_.filePathList, true) << std::endl;
  for( const auto& file: _parameters_.filePathList){
    std::string path = GenericToolbox::expandEnvironmentVariables(file);
    LogThrowIf(not GenericToolbox::doesTFileIsValid(path, {_parameters_.treePath}), "Invalid file: " << path);
  }

  this->parseStringParameters();
  this->doEventSelection();
  this->fetchRequestedLeaves();
  this->preAllocateMemory();
  this->readAndFill();

  LogWarning << "Loaded " << getTitle() << std::endl;
}
std::string DataDispenser::getTitle(){
  std::stringstream ss;
  if( _owner_ != nullptr ) ss << _owner_->getName();
  ss << "/" << _parameters_.name;
  return ss.str();
}

void DataDispenser::buildSampleToFillList(){
  LogWarning << "Fetching samples to fill..." << std::endl;

  for( auto& sample : _sampleSetPtrToLoad_->getFitSampleList() ){
    if( not sample.isEnabled() ) continue;
    if( sample.isDatasetValid(_owner_->getName()) ){
      _cache_.samplesToFillList.emplace_back(&sample);
    }
  }

  if( _cache_.samplesToFillList.empty() ){
    LogInfo << "No sample selected." << std::endl;
    return;
  }
}
void DataDispenser::parseStringParameters() {

  auto replaceToyIndexFct = [&](std::string& formula_){
    if( GenericToolbox::doesStringContainsSubstring(formula_, "<I_TOY>") ){
      LogThrowIf(_parameters_.iThrow==-1, "<I_TOY> not set.");
      GenericToolbox::replaceSubstringInsideInputString(formula_, "<I_TOY>", std::to_string(_parameters_.iThrow));
    }
  };
  auto overrideLeavesNamesFct = [&](std::string& formula_){
    for( auto& replaceEntry : _cache_.varsToOverrideList ){
      GenericToolbox::replaceSubstringInsideInputString(formula_, replaceEntry, _parameters_.overrideLeafDict[replaceEntry]);
    }
  };

  if( not _parameters_.overrideLeafDict.empty() ){
    for( auto& entryDict : _parameters_.overrideLeafDict ){ replaceToyIndexFct(entryDict.second); }
    LogInfo << "Overriding leaf dict: " << GenericToolbox::parseMapAsString(_parameters_.overrideLeafDict) << std::endl;

    for( auto& overrideEntry : _parameters_.overrideLeafDict ){
      _cache_.varsToOverrideList.emplace_back(overrideEntry.first);
    }
    // make sure we process the longest words first: "thisIsATest" variable should be replaced before "thisIs"
    GenericToolbox::sortVector(_cache_.varsToOverrideList, [](const std::string& a_, const std::string& b_){ return a_.size() > b_.size(); });
  }

  if( GenericToolbox::Json::doKeyExist(_config_, "variablesTransform") ){
    // load transformations
    int index{0};
    for( auto& varTransform : GenericToolbox::Json::fetchValue<std::vector<nlohmann::json>>(_config_, "variablesTransform") ){
      _cache_.eventVarTransformList.emplace_back( varTransform );
      _cache_.eventVarTransformList.back().setIndex(index++);
      _cache_.eventVarTransformList.back().initialize();
    }
    // sort them according to their output
    GenericToolbox::sortVector(_cache_.eventVarTransformList, [](const EventVarTransformLib& a_, const EventVarTransformLib& b_){
      // does a_ is a self transformation? -> if yes, don't change the order
      if( GenericToolbox::doesElementIsInVector(a_.getOutputVariableName(), a_.fetchRequestedVars()) ){ return false; }
      // does b_ transformation needs a_ output? -> if yes, a needs to go first
      if( GenericToolbox::doesElementIsInVector(a_.getOutputVariableName(), b_.fetchRequestedVars()) ){ return true; }
      // otherwise keep the order from the declaration
      if( a_.getIndex() < b_.getIndex() ) return true;
      // default -> won't change the order
      return false;
    });
  }

  replaceToyIndexFct(_parameters_.dialIndexFormula);
  replaceToyIndexFct(_parameters_.nominalWeightFormulaStr);
  replaceToyIndexFct(_parameters_.selectionCutFormulaStr);

  overrideLeavesNamesFct(_parameters_.dialIndexFormula);
  overrideLeavesNamesFct(_parameters_.nominalWeightFormulaStr);
  overrideLeavesNamesFct(_parameters_.selectionCutFormulaStr);

}
void DataDispenser::doEventSelection(){
  LogWarning << "Performing event selection..." << std::endl;

  LogInfo << "Event selection..." << std::endl;

  ROOT::EnableThreadSafety();

  // how meaning buffers?
  int nThreads{GundamGlobals::getParallelWorker().getNbThreads()};
  if( _owner_->isDevSingleThreadEventSelection() ) { nThreads = 1; }

  // declare the buffers
  std::vector<std::vector<std::vector<bool>>> perThreadEventIsInSamplesList(nThreads);
  std::vector<std::vector<size_t>> perThreadSampleNbOfEvents(nThreads);

  // selection
  auto selectionFct = [&](int iThread_){

    GundamGlobals::getThreadMutex().lock();
    TChain treeChain(_parameters_.treePath.c_str());
    for (const auto &file: _parameters_.filePathList) {
      std::string name = GenericToolbox::expandEnvironmentVariables(file);
      if (name != file and iThread_ == 0) {
        LogWarning << "Filename expanded to: " << name << std::endl;
      }
      treeChain.Add(name.c_str());
    }
    LogThrowIf(treeChain.GetEntries() == 0, "TChain is empty.");

    if( iThread_ == 0 ) LogInfo << "Defining selection formulas..." << std::endl;

    GenericToolbox::LeafCollection lCollection;
    lCollection.setTreePtr( &treeChain );

    int selectionCutLeafFormIndex{-1};

    // global cut
    if( not _parameters_.selectionCutFormulaStr.empty() ){
      selectionCutLeafFormIndex = lCollection.addLeafExpression( _parameters_.selectionCutFormulaStr );
    }

    // sample cuts
    GenericToolbox::TablePrinter tableSelectionCuts;
    tableSelectionCuts.setColTitles({{"Sample"}, {"Selection Cut"}});

    std::vector<std::pair<int, int>> sampleCutIdxList;

    for( size_t iSample = 0; iSample < _cache_.samplesToFillList.size(); iSample++ ){
      auto* samplePtr = _cache_.samplesToFillList[iSample];
      sampleCutIdxList.emplace_back(iSample, -1);

      std::string selectionCut = samplePtr->getSelectionCutsStr();
      for (auto &replaceEntry: _cache_.varsToOverrideList) {
        GenericToolbox::replaceSubstringInsideInputString(
            selectionCut, replaceEntry, _parameters_.overrideLeafDict[replaceEntry]
        );
      }

      if( selectionCut.empty() ){ continue; }

      sampleCutIdxList.back().second = lCollection.addLeafExpression( selectionCut );
      tableSelectionCuts << samplePtr->getName() << GenericToolbox::TablePrinter::Action::NextColumn;
      tableSelectionCuts << selectionCut << GenericToolbox::TablePrinter::Action::NextLine;

    }
    if( iThread_==0 ){ tableSelectionCuts.printTable(); }

    lCollection.initialize();

    GenericToolbox::VariableMonitor readSpeed("bytes");

    // Multi-thread index splitting
    Long64_t nEvents = treeChain.GetEntries();
    Long64_t iGlobal = 0;

    auto bounds = GenericToolbox::ParallelWorker::getThreadBoundIndices( iThread_, nThreads, nEvents );

    // Load the branches
    treeChain.LoadTree( bounds.first );

    // for each event, which sample is active?
    perThreadEventIsInSamplesList[iThread_].resize(nEvents, std::vector<bool>(_cache_.samplesToFillList.size(), true));
    perThreadSampleNbOfEvents[iThread_].resize(_cache_.samplesToFillList.size(), 0);
    std::string progressTitle = "Performing event selection on " + this->getTitle() + "...";
    std::stringstream ssProgressTitle;
    TFile *lastFilePtr{nullptr};
    GundamGlobals::getThreadMutex().unlock();


    for ( Long64_t iEntry = bounds.first ; iEntry < bounds.second ; iEntry++ ) {
      if( iThread_ == 0 ){
        readSpeed.addQuantity(treeChain.GetEntry(iEntry)*nThreads);
        if (GenericToolbox::showProgressBar(iGlobal, nEvents)) {
          ssProgressTitle.str("");

          ssProgressTitle << LogInfo.getPrefixString() << "Read from disk: "
                          << GenericToolbox::padString(GenericToolbox::parseSizeUnits(readSpeed.getTotalAccumulated()), 8) << " ("
                          << GenericToolbox::padString(GenericToolbox::parseSizeUnits(readSpeed.evalTotalGrowthRate()), 8) << "/s)";

          int cpuPercent = int(GenericToolbox::getCpuUsageByProcess());
          ssProgressTitle << " / CPU efficiency: " << GenericToolbox::padString(std::to_string(cpuPercent/nThreads), 3,' ')
                          << "%" << std::endl;

          ssProgressTitle << LogInfo.getPrefixString() << progressTitle;
          GenericToolbox::displayProgressBar(iGlobal, nEvents, ssProgressTitle.str());
        }
        iGlobal += nThreads;
      }
      else{
        treeChain.GetEntry(iEntry);
      }

      if ( selectionCutLeafFormIndex != -1 ){
        if( lCollection.getLeafFormList()[selectionCutLeafFormIndex].evalAsDouble() == 0 ){
          for (size_t iSample = 0; iSample < _cache_.samplesToFillList.size(); iSample++) {
            perThreadEventIsInSamplesList[iThread_][iEntry][iSample] = false;
          }
          if (GundamGlobals::getVerboseLevel() == INLOOP_TRACE) {
            LogTrace << "Event #" << treeChain.GetFileNumber() << ":" << treeChain.GetReadEntry()
                     << " rejected because of " << _parameters_.selectionCutFormulaStr << std::endl;
          }
          continue;
        }
      }

      for( auto& sampleCutIdx : sampleCutIdxList ){
        if( sampleCutIdx.second == -1 ){
          perThreadSampleNbOfEvents[iThread_][sampleCutIdx.first]++;
          if (GundamGlobals::getVerboseLevel() == INLOOP_TRACE) {
            LogDebug << "Event #" << treeChain.GetFileNumber() << ":" << treeChain.GetReadEntry()
                     << " included as sample " << sampleCutIdx.first << " (NO SELECTION CUT)" << std::endl;
          }
          continue;
        }

        if( lCollection.getLeafFormList()[sampleCutIdx.second].evalAsDouble() == 0 ){
          perThreadEventIsInSamplesList[iThread_][iEntry][sampleCutIdx.first] = false;
          if (GundamGlobals::getVerboseLevel() == INLOOP_TRACE) {
            LogTrace << "Event #" << treeChain.GetFileNumber() << ":" << treeChain.GetReadEntry()
                     << " rejected as sample " << sampleCutIdx.first << " because of "
                     << lCollection.getLeafFormList()[sampleCutIdx.second].getSummary() << std::endl;
          }
        }
        else {
          perThreadSampleNbOfEvents[iThread_][sampleCutIdx.first]++;
          if (GundamGlobals::getVerboseLevel() == INLOOP_TRACE) {
            LogDebug << "Event #" << treeChain.GetFileNumber() << ":" << treeChain.GetReadEntry()
                     << " included as sample " << sampleCutIdx.first << " because of "
                     << lCollection.getLeafFormList()[sampleCutIdx.second].getSummary() << std::endl;
          }
        }
      }

    } // iEvent

    if( iThread_ == 0 ){ GenericToolbox::displayProgressBar(nEvents, nEvents, ssProgressTitle.str()); }
  };

  if( not _owner_->isDevSingleThreadEventSelection() ) {
    GundamGlobals::getParallelWorker().addJob(__METHOD_NAME__, selectionFct);
    GundamGlobals::getParallelWorker().runJob(__METHOD_NAME__);
    GundamGlobals::getParallelWorker().removeJob(__METHOD_NAME__);
  }
  else {
    selectionFct(0);
  }


  LogInfo << "Merging thread results" << std::endl;
  _cache_.sampleNbOfEvents.resize(_cache_.samplesToFillList.size(), 0);
  for( size_t iThread = 0 ; iThread < nThreads ; iThread++ ){
    if( _cache_.eventIsInSamplesList.empty() ){
      _cache_.eventIsInSamplesList.resize(perThreadEventIsInSamplesList[iThread].size(), std::vector<bool>(_cache_.samplesToFillList.size(), true));
    }
    for( size_t iEntry = 0 ; iEntry < perThreadEventIsInSamplesList[iThread].size() ; iEntry++ ){
      for( size_t iSample = 0 ; iSample < _cache_.samplesToFillList.size() ; iSample++ ){
        if(not perThreadEventIsInSamplesList[iThread][iEntry][iSample]) _cache_.eventIsInSamplesList[iEntry][iSample] = false;
      }
    }
    for( size_t iSample = 0 ; iSample < _cache_.samplesToFillList.size() ; iSample++ ){
      _cache_.sampleNbOfEvents[iSample] += perThreadSampleNbOfEvents[iThread][iSample];
    }
  }

  if( _owner_->isShowSelectedEventCount() ){
    LogWarning << "Events passing selection cuts:" << std::endl;
    GenericToolbox::TablePrinter t;
    t.setColTitles({{"Sample"}, {"# of events"}});
    for(size_t iSample = 0 ; iSample < _cache_.samplesToFillList.size() ; iSample++ ){
      t.addTableLine({{"\""+_cache_.samplesToFillList[iSample]->getName()+"\""}, std::to_string(_cache_.sampleNbOfEvents[iSample])});
    }
    t.printTable();
  }

}
void DataDispenser::fetchRequestedLeaves(){
  LogWarning << "Poll every objects for requested variables..." << std::endl;

  if( _dialCollectionListPtr_ != nullptr ){
    LogInfo << "Selecting dial collections..." << std::endl;
    for( auto& dialCollection : *_dialCollectionListPtr_ ){
      if( dialCollection.isDatasetValid( _owner_->getName() ) ){
        _cache_.dialCollectionsRefList.emplace_back( &dialCollection );
      }
    }
  }

  if( not _cache_.dialCollectionsRefList.empty() ) {
    std::vector<std::string> indexRequests;
    for( auto& dialCollection : _cache_.dialCollectionsRefList ) {
      if( dialCollection->getApplyConditionFormula() != nullptr ) {
        for( int iPar = 0 ; iPar < dialCollection->getApplyConditionFormula()->GetNpar() ; iPar++ ){
          GenericToolbox::addIfNotInVector(dialCollection->getApplyConditionFormula()->GetParName(iPar), indexRequests);
        }
      }
      if( not dialCollection->getGlobalDialLeafName().empty() ){
        GenericToolbox::addIfNotInVector(dialCollection->getGlobalDialLeafName(), indexRequests);
      }
      for( auto& bin : dialCollection->getDialBinSet().getBinsList() ) {
        for( auto& var : bin.getVariableNameList() ) {
          GenericToolbox::addIfNotInVector(var, indexRequests);
        }
      }
    }
    LogInfo << "DialCollection requests for indexing: " << GenericToolbox::parseVectorAsString(indexRequests) << std::endl;
    for( auto& var : indexRequests ){ _cache_.addVarRequestedForIndexing(var); }
  }

  // sample binning
  if( _sampleSetPtrToLoad_ != nullptr ){
    std::vector<std::string> indexRequests;
    for (auto &sample: _sampleSetPtrToLoad_->getFitSampleList()) {
      for (auto &bin: sample.getBinning().getBinsList()) {
        for (auto &var: bin.getVariableNameList()) {
          GenericToolbox::addIfNotInVector(var, indexRequests);
        }
      }
    }
    LogInfo << "Samples requests for indexing: " << GenericToolbox::parseVectorAsString(indexRequests) << std::endl;
    for( auto& var : indexRequests ){ _cache_.addVarRequestedForIndexing(var); }
  }

  // plotGen
  if( _plotGenPtr_ != nullptr ){
    std::vector<std::string> storeRequests;
    for( auto& var : _plotGenPtr_->fetchListOfVarToPlot(not _parameters_.useMcContainer) ){
      GenericToolbox::addIfNotInVector(var, storeRequests);
    }

    if( _parameters_.useMcContainer ){
      for( auto& var : _plotGenPtr_->fetchListOfSplitVarNames() ){
        GenericToolbox::addIfNotInVector(var, storeRequests);
      }
    }

    LogInfo << "PlotGenerator requests for storage:" << GenericToolbox::parseVectorAsString(storeRequests) << std::endl;
    for (auto &var: storeRequests) { _cache_.addVarRequestedForStorage(var); }
  }

  // storage requested by user
  {
    std::vector<std::string> storeRequests;
    for (auto &additionalLeaf: _parameters_.additionalVarsStorage) {
      GenericToolbox::addIfNotInVector(additionalLeaf, storeRequests);
    }
    LogInfo << "Dataset additional requests for storage:" << GenericToolbox::parseVectorAsString(storeRequests) << std::endl;
    for (auto &var: storeRequests) { _cache_.addVarRequestedForStorage(var); }
  }

  // fit sample set storage requests
  if( _sampleSetPtrToLoad_ != nullptr ){
    std::vector<std::string> storeRequests;
    for (auto &var: _sampleSetPtrToLoad_->getAdditionalVariablesForStorage()) {
      GenericToolbox::addIfNotInVector(var, storeRequests);
    }
    LogInfo << "SampleSet additional request for storage:" << GenericToolbox::parseVectorAsString(storeRequests) << std::endl;
    for (auto &var: storeRequests) { _cache_.addVarRequestedForStorage(var); }
  }

  // transforms inputs
  if( not _cache_.eventVarTransformList.empty() ){
    std::vector<std::string> indexRequests;
    for( int iTrans = int(_cache_.eventVarTransformList.size())-1 ; iTrans >= 0 ; iTrans-- ){
      // in reverse order -> Treat the highest level vars first (they might need lower level variables)
      std::string outVarName = _cache_.eventVarTransformList[iTrans].getOutputVariableName();
      if( GenericToolbox::doesElementIsInVector( outVarName, _cache_.varsRequestedForIndexing )
          or GenericToolbox::doesElementIsInVector( outVarName, indexRequests )
      ){
        // ok it is needed -> activate dependencies
        for( auto& var: _cache_.eventVarTransformList[iTrans].fetchRequestedVars() ){
          GenericToolbox::addIfNotInVector(var, indexRequests);
        }
      }
    }

    LogInfo << "EventVariableTransformation requests for indexing: " << GenericToolbox::parseVectorAsString(indexRequests) << std::endl;
    for( auto& var : indexRequests ){ _cache_.addVarRequestedForIndexing(var); }
  }

  LogInfo << "Vars requested for indexing: " << GenericToolbox::parseVectorAsString(_cache_.varsRequestedForIndexing, false) << std::endl;
  LogInfo << "Vars requested for storage: " << GenericToolbox::parseVectorAsString(_cache_.varsRequestedForStorage, false) << std::endl;

  // Now build the var to leaf translation
  for( auto& var : _cache_.varsRequestedForIndexing ){
    _cache_.varToLeafDict[var].first = var;    // default is the same name
    _cache_.varToLeafDict[var].second = false; // is dummy branch?

    // strip brackets
    _cache_.varToLeafDict[var].first = GenericToolbox::stripBracket(_cache_.varToLeafDict[var].first, '[', ']');

    // look for override requests
    if( GenericToolbox::doesKeyIsInMap(_cache_.varToLeafDict[var].first, _parameters_.overrideLeafDict) ){
      // leafVar will actually be the overrided leaf name while event will keep the original name
      _cache_.varToLeafDict[var].first = _parameters_.overrideLeafDict[_cache_.varToLeafDict[var].first];
      _cache_.varToLeafDict[var].first = GenericToolbox::stripBracket(_cache_.varToLeafDict[var].first, '[', ']');
    }

    // possible dummy ?
    // [OUT] variables only
    // [OUT] not requested by its inputs
    for( auto& varTransform : _cache_.eventVarTransformList ){
      std::string outVarName = varTransform.getOutputVariableName();
      if( outVarName != var ) continue;
      if( GenericToolbox::doesElementIsInVector(outVarName, varTransform.fetchRequestedVars()) ) continue;
      _cache_.varToLeafDict[var].second = true;
      break;
    }
  }

//  GenericToolbox::TablePrinter t;
//  t.setColTitles({"Variable name", "Leaf name", "From VarTransform?"});
//  for( auto& varToLeafDictEntry : _cache_.varToLeafDict ){
//    std::string colorCode{};
//    if( GenericToolbox::doesElementIsInVector(varToLeafDictEntry.first, _cache_.varsRequestedForStorage) ){ colorCode = GenericToolbox::ColorCodes::blueBackground; }
//    t.addTableLine({varToLeafDictEntry.first, varToLeafDictEntry.second.first, std::to_string(varToLeafDictEntry.second.second)}, colorCode);
//  }
//  t.printTable();

}
void DataDispenser::preAllocateMemory(){
  LogInfo << "Pre-allocating memory..." << std::endl;
  /// \brief The following lines are necessary since the events might get
  /// resized while being in multithread Because std::vector is insuring
  /// continuous memory allocation, a resize sometimes lead to the full moving
  /// of a vector memory. This is not thread safe, so better ensure the vector
  /// won't have to do this by allocating the right event size.

  // MEMORY CLAIM?
  TChain treeChain(_parameters_.treePath.c_str());
  for( const auto& file: _parameters_.filePathList){
    std::string name = GenericToolbox::expandEnvironmentVariables(file);
    if (name != file) {
      LogWarning << "Filename expanded to: " << name << std::endl;
    }
    treeChain.Add(name.c_str());
  }

  GenericToolbox::LeafCollection lCollection;
  lCollection.setTreePtr( &treeChain );
  for( auto& var : _cache_.varsRequestedForIndexing ){
    // look for override requests
    lCollection.addLeafExpression(
        GenericToolbox::doesKeyIsInMap(var, _parameters_.overrideLeafDict) ?
        _parameters_.overrideLeafDict[var]: var
    );
  }
  lCollection.initialize();

  PhysicsEvent eventPlaceholder;
  eventPlaceholder.setDataSetIndex(_owner_->getDataSetIndex());
  eventPlaceholder.setCommonVarNameListPtr(std::make_shared<std::vector<std::string>>(_cache_.varsRequestedForStorage));

  std::vector<const GenericToolbox::LeafForm*> leafFormToVarList{};
  for( auto& storageVar : *eventPlaceholder.getCommonVarNameListPtr() ){
    leafFormToVarList.emplace_back( lCollection.getLeafFormPtr(
        GenericToolbox::doesKeyIsInMap(storageVar, _parameters_.overrideLeafDict) ?
        _parameters_.overrideLeafDict[storageVar]: storageVar
    ));
  }

  eventPlaceholder.allocateMemory( leafFormToVarList );

//  // Just a placeholder for creating the dictionary
//  auto tBuf = this->generateTreeEventBuffer(&treeChain, _cache_.varsRequestedForStorage);
//  auto copyDict = eventPlaceholder.generateDict(tBuf, _parameters_.overrideLeafDict);
//  eventPlaceholder.copyData(copyDict);

  LogInfo << "Reserving event memory..." << std::endl;
  _cache_.sampleIndexOffsetList.resize(_cache_.samplesToFillList.size());
  _cache_.sampleEventListPtrToFill.resize(_cache_.samplesToFillList.size());
  for( size_t iSample = 0 ; iSample < _cache_.sampleNbOfEvents.size() ; iSample++ ){
    auto* container = &_cache_.samplesToFillList[iSample]->getDataContainer();
    if(_parameters_.useMcContainer) container = &_cache_.samplesToFillList[iSample]->getMcContainer();

    _cache_.sampleEventListPtrToFill[iSample] = &container->eventList;
    _cache_.sampleIndexOffsetList[iSample] = _cache_.sampleEventListPtrToFill[iSample]->size();
    container->reserveEventMemory(_owner_->getDataSetIndex(), _cache_.sampleNbOfEvents[iSample], eventPlaceholder);
  }

  size_t nEvents = treeChain.GetEntries();
  if( _eventDialCacheRef_ != nullptr ){
    // DEV
    if( not _cache_.dialCollectionsRefList.empty() ){
      LogInfo << "Creating slots for event-by-event dials..." << std::endl;
      size_t nDialsMaxPerEvent{0};
      for( auto& dialCollection : _cache_.dialCollectionsRefList ){
        LogScopeIndent;
        nDialsMaxPerEvent += 1;
        if( dialCollection->isBinned() ){
          // Filling var indexes for faster eval with PhysicsEvent:
          for( auto& bin : dialCollection->getDialBinSet().getBinsList() ){
            std::vector<int> varIndexes;
            for( auto& var : bin.getVariableNameList() ){
              varIndexes.emplace_back(
                  GenericToolbox::findElementIndex(
                      var, _cache_.varsRequestedForIndexing));
            }
            bin.setEventVarIndexCache(varIndexes);
          }
        }
        else if( not dialCollection->getGlobalDialLeafName().empty() ){
          // Reserve memory for additional dials (those on a tree leaf)
          auto dialType = dialCollection->getGlobalDialType();
          LogInfo << dialCollection->getTitle() << ": creating " << nEvents;
          LogInfo << " slots for " << dialType << std::endl;

          dialCollection->getDialBaseList().clear();
          dialCollection->getDialBaseList().resize(nEvents);
        }
        else{
          LogThrow("DEV ERROR: not binned, not event-by-event?");
        }
      }
      _eventDialCacheRef_->allocateCacheEntries(nEvents, nDialsMaxPerEvent);
    }
    else{
      // all events should be referenced in the cache
      _eventDialCacheRef_->allocateCacheEntries(nEvents, 0);
    }
  }
}
void DataDispenser::readAndFill(){
  LogWarning << "Reading dataset and loading..." << std::endl;

  if( not _parameters_.nominalWeightFormulaStr.empty() ){
    LogInfo << "Nominal weight: \"" << _parameters_.nominalWeightFormulaStr << "\"" << std::endl;
  }
  if( not _parameters_.dialIndexFormula.empty() ){
    LogInfo << "Dial index for TClonesArray: \"" << _parameters_.dialIndexFormula << "\"" << std::endl;
  }

  LogWarning << "Loading and indexing..." << std::endl;
  if(not _owner_->isDevSingleThreadEventLoaderAndIndexer() and GundamGlobals::getParallelWorker().getNbThreads() > 1 ){
    ROOT::EnableThreadSafety();
    std::function<void(int)> f = [&](int iThread_){ this->fillFunction(iThread_); };
    GundamGlobals::getParallelWorker().addJob(__METHOD_NAME__, f);
    GundamGlobals::getParallelWorker().runJob(__METHOD_NAME__);
    GundamGlobals::getParallelWorker().removeJob(__METHOD_NAME__);
  }
  else{
    this->fillFunction(-1); // for better debug breakdown
  }

  LogInfo << "Shrinking lists..." << std::endl;
  for( size_t iSample = 0 ; iSample < _cache_.samplesToFillList.size() ; iSample++ ){
    auto* container = &_cache_.samplesToFillList[iSample]->getDataContainer();
    if(_parameters_.useMcContainer) container = &_cache_.samplesToFillList[iSample]->getMcContainer();
    container->shrinkEventList(_cache_.sampleIndexOffsetList[iSample]);
  }

//  if( _owner_->isSortLoadedEvents() ){
//    LogWarning << "Re-sorting loaded events..." << std::endl;
//    for( auto& evList : _cache_.sampleEventListPtrToFill ){
//      GenericToolbox::sortVector(*evList, [](const PhysicsEvent& a, const PhysicsEvent& b){
//        if( a.getDataSetIndex() < b.getDataSetIndex() ) { return true; }
//        if( a.getEntryIndex() < b.getEntryIndex() ) { return true; }
//        if( a.getEntryIndex() == b.getEntryIndex() and a.getDataSetIndex() == b.getDataSetIndex() ){ return false; }
//        return false;
//      });
//    }
//  }

}
void DataDispenser::loadFromHistContent(){
  LogWarning << "Creating dummy PhysicsEvent entries for loading hist content" << std::endl;

  // non-trivial as we need to propagate systematics. Need to merge with the original data loader, but not straight forward?
  LogThrowIf( _parameters_.useMcContainer, "Hist loader not implemented for MC containers" );

  // counting events
  _cache_.sampleNbOfEvents.resize(_cache_.samplesToFillList.size());
  _cache_.sampleIndexOffsetList.resize(_cache_.samplesToFillList.size());
  _cache_.sampleEventListPtrToFill.resize(_cache_.samplesToFillList.size());


  PhysicsEvent eventPlaceholder;
  eventPlaceholder.setDataSetIndex(_owner_->getDataSetIndex());
  eventPlaceholder.setEventWeight(0); // default.

  // claiming event memory
  for( size_t iSample = 0 ; iSample < _cache_.samplesToFillList.size() ; iSample++ ){

    eventPlaceholder.setCommonVarNameListPtr(
        std::make_shared<std::vector<std::string>>(_cache_.samplesToFillList[iSample]->getBinning().getBinVariables())
    );
    for( size_t iVar = 0 ; iVar < _cache_.samplesToFillList[iSample]->getBinning().getBinVariables().size() ; iVar++ ){
      eventPlaceholder.getVarHolderList()[iVar].emplace_back(double(0.) );
    }
    eventPlaceholder.resizeVarToDoubleCache();

    // one event per bin
    _cache_.sampleNbOfEvents[iSample] = _cache_.samplesToFillList[iSample]->getBinning().getBinsList().size();

    // fetch event container
    auto* container = &_cache_.samplesToFillList[iSample]->getDataContainer();

    _cache_.sampleEventListPtrToFill[iSample] = &container->eventList;
    _cache_.sampleIndexOffsetList[iSample] = _cache_.sampleEventListPtrToFill[iSample]->size();
    container->reserveEventMemory( _owner_->getDataSetIndex(), _cache_.sampleNbOfEvents[iSample], eventPlaceholder );

    // indexing according to the binning
    for( size_t iEvent=_cache_.sampleIndexOffsetList[iSample] ; iEvent < container->eventList.size() ; iEvent++ ){
      container->eventList[iEvent].setSampleBinIndex( int( iEvent ) );
    }
  }

  LogInfo << "Reading external hist files..." << std::endl;

  // read hist content from file
  TFile* fHist{nullptr};
  LogThrowIf( not GenericToolbox::Json::doKeyExist(_parameters_.fromHistContent, "fromRootFile"), "No root file provided." );
  auto filePath = GenericToolbox::Json::fetchValue<std::string>(_parameters_.fromHistContent, "fromRootFile");

  LogInfo << "Opening: " << filePath << std::endl;

  LogThrowIf( not GenericToolbox::doesTFileIsValid(filePath), "Could not open file: " << filePath );
  fHist = TFile::Open(filePath.c_str());
  LogThrowIf(fHist == nullptr, "Could not open file: " << filePath);

  LogThrowIf( not GenericToolbox::Json::doKeyExist(_parameters_.fromHistContent, "sampleList"), "Could not find samplesList." );
  auto sampleList = GenericToolbox::Json::fetchValue<nlohmann::json>(_parameters_.fromHistContent, "sampleList");
  for( auto& sample : _cache_.samplesToFillList ){
    LogScopeIndent;

    auto entry = GenericToolbox::Json::fetchMatchingEntry( sampleList, "name", sample->getName() );
    LogContinueIf( entry.empty(), "Could not find sample histogram: " << sample->getName() );

    LogThrowIf( not GenericToolbox::Json::doKeyExist( entry, "hist" ), "No hist name provided for " << sample->getName() );
    auto histName = GenericToolbox::Json::fetchValue<std::string>( entry, "hist" );
    LogInfo << "Filling sample \"" << sample->getName() << "\" using hist with name: " << histName << std::endl;

    LogThrowIf( not GenericToolbox::Json::doKeyExist( entry, "axis" ), "No axis names provided for " << sample->getName() );
    auto axisNameList = GenericToolbox::Json::fetchValue<std::vector<std::string>>(entry, "axis");

    auto* hist = fHist->Get<THnD>( histName.c_str() );
    LogThrowIf( hist == nullptr, "Could not find THnD \"" << histName << "\" within " << fHist->GetPath() );

    int nBins = 1;
    for( int iDim = 0 ; iDim < hist->GetNdimensions() ; iDim++ ){
      nBins *= hist->GetAxis(iDim)->GetNbins();
    }

    LogAlertIf( nBins != int( sample->getBinning().getBinsList().size() ) ) <<
      "Mismatching bin number for " << sample->getName() << ":" << std::endl
      << GET_VAR_NAME_VALUE(nBins) << std::endl
      << GET_VAR_NAME_VALUE(sample->getBinning().getBinsList().size()) << std::endl;

    auto* container = &sample->getDataContainer();
    for( size_t iBin = 0 ; iBin < sample->getBinning().getBinsList().size() ; iBin++ ){
      auto target = sample->getBinning().getBinsList()[iBin].generateBinTarget( axisNameList );
      auto histBinIndex = hist->GetBin( target.data() ); // bad fetch..?

      container->eventList[iBin].setSampleIndex( sample->getIndex() );
      for( size_t iVar = 0 ; iVar < target.size() ; iVar++ ){
        container->eventList[iBin].setVariable( target[iVar], axisNameList[iVar] );
      }
      container->eventList[iBin].setBaseWeight(hist->GetBinContent(histBinIndex));
      container->eventList[iBin].resetEventWeight();
    }

  }

  fHist->Close();
}

void DataDispenser::fillFunction(int iThread_){
//  std::scoped_lock<std::mutex> l(_mutex_);

  int nThreads = GundamGlobals::getParallelWorker().getNbThreads();
  if( iThread_ == -1 ){ iThread_ = 0; nThreads = 1; } // special mode

  TChain treeChain(_parameters_.treePath.c_str());
  for( const auto& file: _parameters_.filePathList){
    std::string name = GenericToolbox::expandEnvironmentVariables(file);
    if (name != file and iThread_ == 0) {
      LogWarning << "Filename expanded to: " << name << std::endl;
    }
    treeChain.Add(name.c_str());
  }

  GenericToolbox::LeafCollection lCollection;
  lCollection.setTreePtr( &treeChain );

  // nominal weight
  TTreeFormula* nominalWeightTreeFormula{nullptr};
  if( not _parameters_.nominalWeightFormulaStr.empty() ){
    auto idx = size_t(lCollection.addLeafExpression( _parameters_.nominalWeightFormulaStr ));
    nominalWeightTreeFormula = (TTreeFormula*) idx; // tweaking types. Ptr will be attributed after init
  }

  // dial array index
  TTreeFormula* dialIndexTreeFormula{nullptr};
  int dialArrayIndex{0};
  if( not _parameters_.dialIndexFormula.empty() ){
    auto idx = size_t(lCollection.addLeafExpression( _parameters_.dialIndexFormula ));
    dialIndexTreeFormula = (TTreeFormula*) idx; // tweaking types. Ptr will be attributed after init
  }


  // variables definition
  std::vector<const GenericToolbox::LeafForm*> leafFormIndexingList{};
  std::vector<const GenericToolbox::LeafForm*> leafFormStorageList{};
  for( auto& var : _cache_.varsRequestedForIndexing ){
    std::string leafExp{var};
    if( GenericToolbox::doesKeyIsInMap( var, _parameters_.overrideLeafDict ) ){
      leafExp = _parameters_.overrideLeafDict[leafExp];
    }
    auto idx = size_t(lCollection.addLeafExpression(leafExp));
    leafFormIndexingList.emplace_back( (GenericToolbox::LeafForm*) idx ); // tweaking types
  }
  for( auto& var : _cache_.varsRequestedForStorage ){
    std::string leafExp{var};
    if( GenericToolbox::doesKeyIsInMap( var, _parameters_.overrideLeafDict ) ){
      leafExp = _parameters_.overrideLeafDict[leafExp];
    }
    auto idx = size_t(lCollection.getLeafExpIndex(leafExp));
    leafFormStorageList.emplace_back( (GenericToolbox::LeafForm*) idx ); // tweaking types
  }

  lCollection.initialize();

  // grab ptr address now
  if( not _parameters_.nominalWeightFormulaStr.empty() ){
    nominalWeightTreeFormula = lCollection.getLeafFormList()[(size_t) nominalWeightTreeFormula].getTreeFormulaPtr().get();
  }
  if( not _parameters_.dialIndexFormula.empty() ){
    dialIndexTreeFormula = lCollection.getLeafFormList()[(size_t) dialIndexTreeFormula].getTreeFormulaPtr().get();
  }
  for( auto& lfInd: leafFormIndexingList ){ lfInd = &(lCollection.getLeafFormList()[(size_t) lfInd]); }
  for( auto& lfSto: leafFormStorageList ){ lfSto = &(lCollection.getLeafFormList()[(size_t) lfSto]); }

  // Event Var Transform
  auto eventVarTransformList = _cache_.eventVarTransformList; // copy for cache
  std::vector<EventVarTransformLib*> varTransformForIndexingList;
  std::vector<EventVarTransformLib*> varTransformForStorageList;
  for( auto& eventVarTransform : eventVarTransformList ){
    if( GenericToolbox::doesElementIsInVector(eventVarTransform.getOutputVariableName(), _cache_.varsRequestedForIndexing) ){
      varTransformForIndexingList.emplace_back(&eventVarTransform);
    }
    if( GenericToolbox::doesElementIsInVector(eventVarTransform.getOutputVariableName(), _cache_.varsRequestedForStorage) ){
      varTransformForStorageList.emplace_back(&eventVarTransform);
    }
  }

  if( iThread_ == 0 ){
    if( not varTransformForIndexingList.empty() ){
      LogInfo << "EventVarTransformLib used for indexing: "
              << GenericToolbox::iterableToString(
                  varTransformForIndexingList,
                  [](const EventVarTransformLib* elm_){ return "\"" + elm_->getTitle() + "\"";}, false)
              << std::endl;
    }
    if( not varTransformForStorageList.empty() ){
      LogInfo << "EventVarTransformLib used for storage: "
              << GenericToolbox::iterableToString(
                  varTransformForStorageList,
                  []( const EventVarTransformLib* elm_){ return "\"" + elm_->getTitle() + "\""; }, false)
              << std::endl;
    }
  }

  // buffer that will store the data for indexing
  PhysicsEvent eventIndexingBuffer;
  eventIndexingBuffer.setDataSetIndex(_owner_->getDataSetIndex());
  eventIndexingBuffer.setCommonVarNameListPtr(std::make_shared<std::vector<std::string>>(_cache_.varsRequestedForIndexing));
  eventIndexingBuffer.allocateMemory(leafFormIndexingList);

  PhysicsEvent eventStorageBuffer;
  eventStorageBuffer.setDataSetIndex(_owner_->getDataSetIndex());
  eventStorageBuffer.setCommonVarNameListPtr(std::make_shared<std::vector<std::string>>(_cache_.varsRequestedForStorage));
  eventStorageBuffer.allocateMemory(leafFormStorageList);

  if(iThread_ == 0){
    LogInfo << "Feeding event variables with:" << std::endl;
    GenericToolbox::TablePrinter table;

    table << "Variable" << GenericToolbox::TablePrinter::NextColumn;
    table << "LeafForm" << GenericToolbox::TablePrinter::NextColumn;
    table << "Transforms" << GenericToolbox::TablePrinter::NextLine;

    for( size_t iVar = 0 ; iVar < _cache_.varsRequestedForIndexing.size() ; iVar++ ){
      std::string var = _cache_.varsRequestedForIndexing[iVar];

      // line color?
      if( GenericToolbox::doesElementIsInVector(var, _cache_.varsRequestedForStorage)){
        table.setColorBuffer(GenericToolbox::ColorCodes::blueBackground);
      }
      else if(
             leafFormIndexingList[iVar]->getLeafTypeName() == "TClonesArray"
          or leafFormIndexingList[iVar]->getLeafTypeName() == "TGraph"
          ){
        table.setColorBuffer( GenericToolbox::ColorCodes::magentaBackground );
      }

      table << var << GenericToolbox::TablePrinter::NextColumn;

      table << leafFormIndexingList[iVar]->getPrimaryExprStr() << "/" << leafFormIndexingList[iVar]->getLeafTypeName();
      table << GenericToolbox::TablePrinter::NextColumn;

      std::vector<std::string> transformsList;
      for( auto* varTransformForIndexing : varTransformForIndexingList ){
        if( varTransformForIndexing->getOutputVariableName() == var ){
          transformsList.emplace_back(varTransformForIndexing->getTitle());
        }
      }
      table << GenericToolbox::parseVectorAsString(transformsList) << GenericToolbox::TablePrinter::NextColumn;
    }


    table.printTable();

    // printing legend
    LogInfo(Logger::Color::BG_BLUE)    << "      " << Logger::getColorEscapeCode(Logger::Color::RESET) << " -> Variables stored in RAM" << std::endl;
    LogInfo(Logger::Color::BG_MAGENTA) << "      " << Logger::getColorEscapeCode(Logger::Color::RESET) << " -> Dials stored in RAM" << std::endl;
  }

  // Will keep track of a picked event pointer
  PhysicsEvent* eventPtr{nullptr};

  size_t sampleEventIndex;
  int threadDialIndex;

  // Loop vars
  bool foundValidDialAmongTheSet{true};
  int lastFailedBinVarIndex{-1}; int lastEventVarIndex{-1};
  const std::pair<double, double>* lastEdges{nullptr};
  size_t iVar{0};
  size_t iSample{0}, nSample{_cache_.samplesToFillList.size()};
  size_t iTransform{0}, nTransform{_cache_.eventVarTransformList.size()};

  // Dials
  size_t eventDialOffset;
  size_t iDialSet, iDial;
  size_t nBinEdges;
  TObject* dialObjectPtr{nullptr};

  // Bin searches
  const std::vector<DataBin>* binsListPtr;
  std::vector<DataBin>::const_iterator binFoundItr;
  auto isBinValid = [&](const DataBin& b_){
    for( iVar = 0 ; iVar < b_.getVariableNameList().size() ; iVar++ ){
      if( not b_.isBetweenEdges(iVar, eventIndexingBuffer.getVarAsDouble(b_.getVariableNameList()[iVar])) ){
        return false;
      }
    } // Var
    return true;
  };

  // Dial bin search
  DataBin* dataBin{nullptr};
  size_t freeSlotDial{0};
  size_t iCollection(-1);
  EventDialCache::IndexedEntry_t* eventDialCacheEntry;
  std::vector<DataBin>::iterator dial2FoundItr;
  auto isDial2Valid = [&](const DataBin& d_){
    nBinEdges = d_.getEdgesList().size();
    for( iVar = 0 ; iVar < nBinEdges ; iVar++ ){
      if( not d_.isBetweenEdges(
          d_.getEdgesList()[iVar],
          eventIndexingBuffer.getVarAsDouble(d_.getEventVarIndexCache()[iVar] ) )
          ){
        return false;
      }
    }
    return true;
  };

  // Formula
  std::vector<std::shared_ptr<TFormula>> varSelectionFormulaList{};
  for( auto* sample : _cache_.samplesToFillList ){
    varSelectionFormulaList.emplace_back(nullptr);
    if( not sample->getVarSelectionFormulaStr().empty() ){
      varSelectionFormulaList.back() = std::make_shared<TFormula>(
          Form("%s_%i_VarSelectionFormula", sample->getName().c_str(), iThread_),
          sample->getVarSelectionFormulaStr().c_str()
      );
      LogInfo(iThread_==0) << "Var selection formula for " << sample->getName() << ": \"" << sample->getVarSelectionFormulaStr() << "\"" << std::endl;
    }
  }

  // Try to read TTree the closest to sequentially possible
  Long64_t nEvents = treeChain.GetEntries();
  Long64_t iGlobal = 0;

  auto bounds = GenericToolbox::ParallelWorker::getThreadBoundIndices(
      iThread_, nThreads, nEvents
      );

  // to generate dials
  DialBaseFactory factory;

  // Load the branches
  treeChain.LoadTree(bounds.first);

  // IO speed monitor
  GenericToolbox::VariableMonitor readSpeed("bytes");
  Int_t nBytes;

  std::string progressTitle = "Loading and indexing...";
  std::stringstream ssProgressBar;

  for(Long64_t iEntry = bounds.first ; iEntry < bounds.second; iEntry++ ){

    if( iThread_ == 0 ){
      if( GenericToolbox::showProgressBar(iGlobal, nEvents) ){

        ssProgressBar.str("");

        ssProgressBar << LogInfo.getPrefixString() << "Reading from disk: "
                      << GenericToolbox::padString(GenericToolbox::parseSizeUnits(readSpeed.getTotalAccumulated()), 8) << " ("
                      << GenericToolbox::padString(GenericToolbox::parseSizeUnits(readSpeed.evalTotalGrowthRate()), 8) << "/s)";

        int cpuPercent = int(GenericToolbox::getCpuUsageByProcess());
        ssProgressBar << " / CPU efficiency: " << GenericToolbox::padString(std::to_string(cpuPercent/nThreads), 3,' ')
                      << "% / RAM: " << GenericToolbox::parseSizeUnits( double(GenericToolbox::getProcessMemoryUsage()) ) << std::endl;

        ssProgressBar << LogInfo.getPrefixString() << progressTitle;
        GenericToolbox::displayProgressBar(iGlobal, nEvents, ssProgressBar.str());
      }
      iGlobal += nThreads;
    }

    bool skipEvent = true;
    for( bool isInSample : _cache_.eventIsInSamplesList[iEntry] ){
      if( isInSample ){ skipEvent = false; break; }
    }
    if( skipEvent ) continue;

    nBytes = treeChain.GetEntry(iEntry);

    // monitor
    if( iThread_ == 0 ){
      readSpeed.addQuantity(nBytes * nThreads);
    }

    if( nominalWeightTreeFormula != nullptr ){
      eventIndexingBuffer.setBaseWeight(nominalWeightTreeFormula->EvalInstance());
      if(eventIndexingBuffer.getBaseWeight() < 0 ){
        LogError << "Negative nominal weight:" << std::endl;

        LogError << "Event buffer is: " << eventIndexingBuffer.getSummary() << std::endl;

        LogError << "Formula leaves:" << std::endl;
        for( int iLeaf = 0 ; iLeaf < nominalWeightTreeFormula->GetNcodes() ; iLeaf++ ){
          if( nominalWeightTreeFormula->GetLeaf(iLeaf) == nullptr ) continue; // for "Entry$" like dummy leaves
          LogError << "Leaf: " << nominalWeightTreeFormula->GetLeaf(iLeaf)->GetName() << "[0] = " << nominalWeightTreeFormula->GetLeaf(iLeaf)->GetValue(0) << std::endl;
        }

        LogThrow("Negative nominal weight");
      }
      if(eventIndexingBuffer.getBaseWeight() == 0 ){
        continue;
      } // skip this event
    }

    for( iSample = 0 ; iSample < nSample ; iSample++ ){
      if( _cache_.eventIsInSamplesList[iEntry][iSample] ){

        // Reset bin index of the buffer
        eventIndexingBuffer.setSampleBinIndex(-1);

        // Getting loaded data in tEventBuffer
        eventIndexingBuffer.copyData( leafFormIndexingList );

        // Propagate transformations for indexing
        for( auto* varTransformPtr : varTransformForIndexingList ){
          varTransformPtr->evalAndStore(eventIndexingBuffer);
        }

        // Sample variable
        if( varSelectionFormulaList[iSample] != nullptr ){
          if(eventIndexingBuffer.evalFormula(varSelectionFormulaList[iSample].get() ) == 0 ) break;
        }

        // Has valid bin?
        binsListPtr = &_cache_.samplesToFillList[iSample]->getBinning().getBinsList();
        binFoundItr = std::find_if(
            binsListPtr->begin(),
            binsListPtr->end(),
            isBinValid
        );

        if (binFoundItr == binsListPtr->end()) {
          // Invalid bin -> next sample
          break;
        }

        // found the bin
        eventIndexingBuffer.setSampleBinIndex(int(std::distance(binsListPtr->begin(), binFoundItr)));

        // OK, now we have a valid fit bin. Let's claim an index.
        GundamGlobals::getThreadMutex().lock();
        // EXTRA LOCK HERE:
        // internal lock seems to not be sufficient when catching a event dial cache entry...
        sampleEventIndex = _cache_.sampleIndexOffsetList[iSample]++;
        if(_eventDialCacheRef_ != nullptr) eventDialCacheEntry = _eventDialCacheRef_->fetchNextCacheEntry();
        GundamGlobals::getThreadMutex().unlock();

        // Get the next free event in our buffer
        eventPtr = &(*_cache_.sampleEventListPtrToFill[iSample])[sampleEventIndex];
        eventPtr->copyData( leafFormStorageList ); // buffer has the right size already

        // Propagate transformation for storage -> use the previous results calculated for indexing
        for( auto* varTransformPtr : varTransformForStorageList ){
          varTransformPtr->storeCachedOutput(*eventPtr);
        }

        eventPtr->setEntryIndex(iEntry);
        eventPtr->setSampleBinIndex(eventIndexingBuffer.getSampleBinIndex());
        eventPtr->setBaseWeight(eventIndexingBuffer.getBaseWeight());
        eventPtr->setNominalWeight(eventIndexingBuffer.getBaseWeight());
        eventPtr->setSampleIndex(_cache_.samplesToFillList[iSample]->getIndex());
        eventPtr->resetEventWeight();

        dialArrayIndex = (dialIndexTreeFormula == nullptr ? 0 : int(dialIndexTreeFormula->EvalInstance()));

//        if( dialArrayIndex != 0 ){
//          LogDebug << std::endl << std::endl;
//          LogDebug << GET_VAR_NAME_VALUE(dialArrayIndex) << std::endl;
//          LogDebug << GET_VAR_NAME_VALUE(iEntry) << std::endl;
//          LogDebug << lCollection.getSummary() << std::endl;
//          eventIndexingBuffer.print();
//          exit(0);
//        }

        // Now the event is ready. Let's index the dials:
        eventDialOffset = 0;

        if( _eventDialCacheRef_ != nullptr ) {
          // there should always be a cache entry even if no dials are applied.
          // This cache is actually used to write MC events with dials in output tree
          eventDialCacheEntry->event.sampleIndex = std::size_t(_cache_.samplesToFillList[iSample]->getIndex());
          eventDialCacheEntry->event.eventIndex = sampleEventIndex;

          iCollection = -1;
          for (auto *dialCollectionRef: _cache_.dialCollectionsRefList) {
            iCollection = dialCollectionRef->getIndex();

            if (not dialCollectionRef->isEnabled()) { continue; }

            if (dialCollectionRef->getApplyConditionFormula() != nullptr) {
              if (eventIndexingBuffer.evalFormula(dialCollectionRef->getApplyConditionFormula().get()) == 0) {
                // next dialSet
                continue;
              }
            }

            if     ( dialCollectionRef->isBinned() ){

              // is only one bin with no condition:
              if (dialCollectionRef->getDialBaseList().size() == 1 and dialCollectionRef->getDialBinSet().isEmpty()) {
                // if is it NOT a DialBinned -> this is the one we are
                // supposed to use
                eventDialCacheEntry->dials[eventDialOffset].collectionIndex = iCollection;
                eventDialCacheEntry->dials[eventDialOffset].interfaceIndex = 0;
                eventDialOffset++;
              } else {
                // ---- probably the slowest part of the indexing: ----
                dial2FoundItr = std::find_if(
                    dialCollectionRef->getDialBinSet().getBinsList().begin(),
                    dialCollectionRef->getDialBinSet().getBinsList().end(),
                    isDial2Valid
                );
                // ----------------------------------------------------

                if (dial2FoundItr != dialCollectionRef->getDialBinSet().getBinsList().end()) {
                  // found DIAL -> get index
                  eventDialCacheEntry->dials[eventDialOffset].collectionIndex = iCollection;
                  eventDialCacheEntry->dials[eventDialOffset].interfaceIndex = std::distance(
                      dialCollectionRef->getDialBinSet().getBinsList().begin(), dial2FoundItr
                  );
                  eventDialOffset++;
                } else {
                  // dial not valid
                }
              }
            }
            else if( not dialCollectionRef->getGlobalDialLeafName().empty() ){
              // Event-by-event dial?
              // grab the dial as a general TObject -> let the factory figure out what to do with it
              dialObjectPtr = (TObject*) *((TObject**) eventIndexingBuffer.getVariableAddress( dialCollectionRef->getGlobalDialLeafName() ));

              // Extra-step for selecting the right dial with TClonesArray
              if( not strcmp(dialObjectPtr->ClassName(), "TClonesArray") ){
                dialObjectPtr = ((TClonesArray*) dialObjectPtr)->At( dialArrayIndex );
              }

              // Do the unique_ptr dance so that memory gets deleted if
              // there is an exception (being stupidly paranoid).
              std::unique_ptr<DialBase> dialBase(
                  factory.makeDial(
                      dialCollectionRef->getTitle(),
                      dialCollectionRef->getGlobalDialType(),
                      dialCollectionRef->getGlobalDialSubType(),
                      dialObjectPtr,
                      dialCollectionRef->useCachedDials()
                  )
              );

              // dialBase is valid -> store it
              if( dialBase != nullptr ){
                freeSlotDial = dialCollectionRef->getNextDialFreeSlot();
                dialBase->setAllowExtrapolation(dialCollectionRef->isAllowDialExtrapolation());
                dialCollectionRef->getDialBaseList()[freeSlotDial] = DialCollection::DialBaseObject(dialBase.release());
                eventDialCacheEntry->dials[eventDialOffset].collectionIndex = iCollection;
                eventDialCacheEntry->dials[eventDialOffset].interfaceIndex = freeSlotDial;
                eventDialOffset++;
              }
            }
            else {
              LogThrow("neither an event by event dial, nor a binned dial");
            }

          }
        }
        else{
          // it is "data"
        }
      } // event has passed the selection?
    } // samples
  } // entries
  if( iThread_ == 0 ){
    GenericToolbox::displayProgressBar(nEvents, nEvents, ssProgressBar.str());
  }

}

//  A Lesser GNU Public License

//  Copyright (C) 2023 GUNDAM DEVELOPERS

//  This library is free software; you can redistribute it and/or
//  modify it under the terms of the GNU Lesser General Public
//  License as published by the Free Software Foundation; either
//  version 2.1 of the License, or (at your option) any later version.

//  This library is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  Lesser General Public License for more details.

//  You should have received a copy of the GNU Lesser General Public
//  License along with this library; if not, write to the
//
//  Free Software Foundation, Inc.
//  51 Franklin Street, Fifth Floor,
//  Boston, MA  02110-1301  USA

// Local Variables:
// mode:c++
// c-basic-offset:2
// compile-command:"$(git rev-parse --show-toplevel)/cmake/gundam-build.sh"
// End:
