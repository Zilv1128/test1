/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT-style license found in the
 * LICENSE file in the root directory of this source tree.
 *
 */

/**
 * User guide
 * ----------
 *
 * 1. Setup the input files:
 * Assuming that you have the acoustic model, language model, features
 * extraction serialized streaming inference DNN, tokens file, lexicon file and
 * input audio file in a directory called modules.
 *  $> ls ~/model
 *   acoustic_model.bin
 *   language.bin
 *   feat.bin
 *   tokens.txt
 *   lexicon.txt
 *
 * $> ls ~/audio
 *   input1.wav
 *   input2.wav
 *
 * 2. Run:
 * multithreaded_wav2letter_example --input_files_base_path ~/model
 *                                  --output_files_base_path /tmp/out
 *      --input_audio_files=${HOME}/audio/input1.wav,${HOME}/audio/inputAudio1.wav
 *
 * For each input file X and output file is written to the
 * output_files_base_path named as X.txt.
 *   $> ls /tmp/out
 *   input1.wav.txt
 *   input2.wav.txt
 *
 *
 */

#include <atomic>
#include <fstream>
#include <istream>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include <cereal/archives/binary.hpp>
#include <cereal/archives/json.hpp>
#include <gflags/gflags.h>

#include "inference/decoder/Decoder.h"
#include "inference/examples/AudioToWords.h"
#include "inference/examples/Util.h"
#include "inference/examples/threadpool/ThreadPool.h"
#include "inference/module/feature/feature.h"
#include "inference/module/module.h"
#include "inference/module/nn/nn.h"

using namespace w2l;
using namespace w2l::streaming;

DEFINE_int32(max_num_threads, 1, "maximum number of threads to use for ASR.");
DEFINE_string(
    input_files_base_path,
    ".",
    "path is added as prefix to input files unless the input file"
    " is a full path.");
DEFINE_string(
    output_files_base_path,
    ".",
    "Output files are saved as [output_files_base_path][input file name].txt");
DEFINE_string(
    feature_module_file,
    "feature_extractor.bin",
    "binary file containing feture module parameters.");
DEFINE_string(
    acoustic_module_file,
    "acoustic_model.bin",
    "binary file containing acoustic module parameters.");
DEFINE_string(
    transitions_file,
    "",
    "binary file containing ASG criterion transition parameters.");
DEFINE_string(tokens_file, "tokens.txt", "text file containing tokens.");
DEFINE_string(lexicon_file, "lexicon.txt", "text file containing lexicon.");
DEFINE_string(
    input_audio_files,
    "",
    "commas separated list of 16KHz wav audio input file to be "
    " traslated to words.");
DEFINE_string(
    input_audio_file_of_paths,
    "",
    "text file with input audio file names. Eavh line should have "
    "an audio file name or a full path to an audio file.");
DEFINE_string(silence_token, "_", "the token to use to denote silence");
DEFINE_string(
    language_model_file,
    "language_model.bin",
    "binary file containing language module parameters.");
DEFINE_string(
    decoder_options_file,
    "decoder_options.json",
    "JSON file containing decoder options"
    " including: max overall beam size, max beam for token selection, beam score threshold"
    ", language model weight, word insertion score, unknown word insertion score"
    ", silence insertion score, and use logadd when merging decoder nodes");

std::string GetInputFileFullPath(const std::string& fileName) {
  return GetFullPath(fileName, FLAGS_input_files_base_path);
}

std::string GetOutputFileFullPath(const std::string& fileName) {
  return GetFullPath(getFileName(fileName), FLAGS_output_files_base_path) +
      ".txt";
}

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  std::vector<std::string> inputFiles;

  if (!FLAGS_input_audio_files.empty()) {
    for (size_t start = 0, pos = 0; pos != std::string::npos; start = pos + 1) {
      // Allow using comma or semicolon (in case user mistakenly used
      // semicolon).
      pos = FLAGS_input_audio_files.find_first_of(",;", start);
      const std::string token =
          FLAGS_input_audio_files.substr(start, pos - start);
      // ignore empty tokens
      if (token.length() > 0) {
        inputFiles.push_back(token);
      }
    }
  }

  if (!FLAGS_input_audio_file_of_paths.empty()) {
    std::ifstream file_of_paths(FLAGS_input_audio_file_of_paths);
    std::string path;
    while (std::getline(file_of_paths, path)) {
      inputFiles.push_back(path);
    }
  }

  const size_t inputFileCount = inputFiles.size();
  std::cout << "Will process " << inputFileCount << " files." << std::endl;

  std::shared_ptr<streaming::Sequential> featureModule;
  std::shared_ptr<streaming::Sequential> acousticModule;

  // Read files
  {
    TimeElapsedReporter feturesLoadingElapsed("features model file loading");
    std::ifstream featFile(
        GetInputFileFullPath(FLAGS_feature_module_file), std::ios::binary);
    if (!featFile.is_open()) {
      throw std::runtime_error(
          "failed to open feature file=" +
          GetInputFileFullPath(FLAGS_feature_module_file) + " for reading");
    }
    cereal::BinaryInputArchive ar(featFile);
    ar(featureModule);
  }

  {
    TimeElapsedReporter acousticLoadingElapsed("acoustic model file loading");
    std::ifstream amFile(
        GetInputFileFullPath(FLAGS_acoustic_module_file), std::ios::binary);
    if (!amFile.is_open()) {
      throw std::runtime_error(
          "failed to open acoustic model file=" +
          GetInputFileFullPath(FLAGS_feature_module_file) + " for reading");
    }
    cereal::BinaryInputArchive ar(amFile);
    ar(acousticModule);
  }

  // String both modeles togthers to a single DNN.
  auto dnnModule = std::make_shared<streaming::Sequential>();
  dnnModule->add(featureModule);
  dnnModule->add(acousticModule);

  std::vector<std::string> tokens;
  {
    TimeElapsedReporter acousticLoadingElapsed("tokens file loading");
    std::ifstream tknFile(GetInputFileFullPath(FLAGS_tokens_file));
    if (!tknFile.is_open()) {
      throw std::runtime_error(
          "failed to open tokens file=" +
          GetInputFileFullPath(FLAGS_tokens_file) + " for reading");
    }
    std::string line;
    while (std::getline(tknFile, line)) {
      tokens.push_back(line);
    }
  }
  int nTokens = tokens.size();
  std::cout << "Tokens loaded - " << nTokens << " tokens" << std::endl;

  fl::lib::text::LexiconDecoderOptions decoderOptions;
  {
    TimeElapsedReporter decoderOptionsElapsed("decoder options file loading");
    std::ifstream decoderOptionsFile(
        GetInputFileFullPath(FLAGS_decoder_options_file));
    if (!decoderOptionsFile.is_open()) {
      throw std::runtime_error(
          "failed to open decoder options file=" +
          GetInputFileFullPath(FLAGS_decoder_options_file) + " for reading");
    }
    cereal::JSONInputArchive ar(decoderOptionsFile);
    // TODO: factor out proper serialization functionality or Cereal
    // specialization.
    ar(cereal::make_nvp("beamSize", decoderOptions.beamSize),
       cereal::make_nvp("beamSizeToken", decoderOptions.beamSizeToken),
       cereal::make_nvp("beamThreshold", decoderOptions.beamThreshold),
       cereal::make_nvp("lmWeight", decoderOptions.lmWeight),
       cereal::make_nvp("wordScore", decoderOptions.wordScore),
       cereal::make_nvp("unkScore", decoderOptions.unkScore),
       cereal::make_nvp("silScore", decoderOptions.silScore),
       cereal::make_nvp("logAdd", decoderOptions.logAdd),
       cereal::make_nvp("criterionType", decoderOptions.criterionType));
  }

  std::vector<float> transitions;
  if (!FLAGS_transitions_file.empty()) {
    TimeElapsedReporter acousticLoadingElapsed("transitions file loading");
    std::ifstream transitionsFile(
        GetInputFileFullPath(FLAGS_transitions_file), std::ios::binary);
    if (!transitionsFile.is_open()) {
      throw std::runtime_error(
          "failed to open transition parameter file=" +
          GetInputFileFullPath(FLAGS_transitions_file) + " for reading");
    }
    cereal::BinaryInputArchive ar(transitionsFile);
    ar(transitions);
  }

  std::shared_ptr<const DecoderFactory> decoderFactory;
  // Create Decoder
  {
    TimeElapsedReporter acousticLoadingElapsed("create decoder");
    decoderFactory = std::make_shared<DecoderFactory>(
        GetInputFileFullPath(FLAGS_tokens_file),
        GetInputFileFullPath(FLAGS_lexicon_file),
        GetInputFileFullPath(FLAGS_language_model_file),
        transitions,
        fl::lib::text::SmearingMode::MAX,
        FLAGS_silence_token,
        0);
  }

  {
    TimeElapsedReporter feturesLoadingElapsed(
        "converting audio input files to text");
    std::cout << "Creating thread pool with " << FLAGS_max_num_threads
              << " threads.\n";
    w2l::streaming::example::ThreadPool pool(FLAGS_max_num_threads);

    std::atomic<int> processedFilesCount = {};
    processedFilesCount = 0;

    for (const std::string& inputFile : inputFiles) {
      const std::string inputFilePath = GetInputFileFullPath(inputFile);
      const std::string outputFilePath = GetOutputFileFullPath(inputFile);

      std::cout << "Enqueue input file=" << inputFile << " to thread pool.\n";
      pool.enqueue(
          [inputFilePath,
           outputFilePath,
           dnnModule,
           decoderFactory,
           &decoderOptions,
           nTokens,
           &processedFilesCount,
           inputFileCount]() -> void {
            const int prossesingFileNumber = ++processedFilesCount;

            std::stringstream stringBuffer;
            stringBuffer << "audioFileToWordsFile() processing "
                         << prossesingFileNumber << "/" << inputFileCount
                         << " input=" << inputFilePath
                         << " output=" << outputFilePath << std::endl;
            std::cout << stringBuffer.str();

            audioFileToWordsFile(
                inputFilePath,
                outputFilePath,
                dnnModule,
                decoderFactory,
                decoderOptions,
                nTokens,
                std::cerr);
          });
    }
  }
}
