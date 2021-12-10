//
// Created by Nadrino on 11/02/2021.
//

#ifndef GUNDAM_GLOBALVARIABLES_H
#define GUNDAM_GLOBALVARIABLES_H

#include <map>
#include <mutex>

#include <TTree.h>
#include <TChain.h>
#include <TRandom3.h>

#include "GenericToolbox.h"
#include "GenericToolbox.ParallelWorker.h"

class GlobalVariables{

public:

  // Setters
  static void setNbThreads(int nbThreads_);
  static void setPrngSeed(ULong_t seed_);

  // Getters
  static bool isEnableDevMode();
  static const int& getNbThreads();
  static std::mutex& getThreadMutex();
  static std::map<std::string, bool>& getBoolMap();
  static std::vector<TChain*>& getChainList();
  static GenericToolbox::ParallelWorker &getParallelWorker();
  static TRandom3& getPrng();

private:

  static bool _enableDevMode_;
  static int _nbThreads_;
  static std::mutex _threadMutex_;
  static std::map<std::string, bool> _boolMap_;
  static std::vector<TChain*> _chainList_;
  static GenericToolbox::ParallelWorker _threadPool_;
  static TRandom3 _prng_;

};

#endif // XSLLHFITTER_GLOBALVARIABLES_H
