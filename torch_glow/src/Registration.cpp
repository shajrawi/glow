/**
 * Copyright (c) Glow Contributors. See CONTRIBUTORS file.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "CachingGraphRunner.h"
#include "GlowFuser.h"
#include "PyTorchCommon.h"
#include "glow/Support/Error.h"

#include <glog/logging.h>
#include <torch/csrc/jit/ir/alias_analysis.h>
#include <torch/csrc/jit/ir/node_hashing.h>
#include <torch/csrc/jit/passes/common_subexpression_elimination.h>
#include <torch/csrc/jit/passes/dead_code_elimination.h>
#include <torch/csrc/jit/passes/pass_manager.h>
#include <torch/csrc/jit/passes/subgraph_rewrite.h>
#include <torch/csrc/jit/passes/utils/subgraph_utils.h>
#include <torch/csrc/jit/runtime/custom_operator.h>
#include <torch/csrc/utils/hash.h>

#include <mutex>
#include <shared_mutex>
#include <signal.h>

namespace glow {

namespace {
/// Lock to protect the global graph runner map.
std::shared_timed_mutex runnerMapMutex;

std::unordered_map<std::string, std::shared_ptr<CachingGraphRunner>> &
getPreloadedRunnerMap() {
  static std::unordered_map<std::string, std::shared_ptr<CachingGraphRunner>>
      preloadedGraphRunners_;
  return preloadedGraphRunners_;
}
} // namespace

size_t getGraphRunnerMapSize() {
  const auto &m = getPreloadedRunnerMap();
  std::shared_lock<std::shared_timed_mutex> rlock(runnerMapMutex);
  return m.size();
}

std::shared_ptr<CachingGraphRunner>
getGraphRunnerForKey(const std::string &key) {
  auto &preloadedRunners = getPreloadedRunnerMap();
  std::shared_lock<std::shared_timed_mutex> rlock(runnerMapMutex);
  auto it = preloadedRunners.find(key);
  if (it == preloadedRunners.end()) {
    return nullptr;
  } else {
    return it->second;
  }
}

std::shared_ptr<CachingGraphRunner>
setGraphRunnerForKey(const std::string &key,
                     std::function<std::shared_ptr<CachingGraphRunner>(void)>
                         graphRunnerBuilder) {
  auto &preloadedRunners = getPreloadedRunnerMap();
  std::unique_lock<std::shared_timed_mutex> wlock(runnerMapMutex);
  const auto it = preloadedRunners.find(key);
  if (it != preloadedRunners.end()) {
    return it->second;
  }

  auto runner = graphRunnerBuilder();
  auto ret = preloadedRunners.emplace(key, runner);
  CHECK(ret.second);
  return runner;
}

bool removeGraphRunnerForKey(const std::string &key) {
  auto &preloadedRunners = getPreloadedRunnerMap();
  std::unique_lock<std::shared_timed_mutex> wlock(runnerMapMutex);
  const auto it = preloadedRunners.find(key);
  if (it == preloadedRunners.end()) {
    return false;
  }
  preloadedRunners.erase(key);
  return true;
}

void registerGlowOp(const c10::Symbol &symbol) {
  torch::jit::RegisterOperators op({torch::jit::Operator(
      symbol,
      [](const torch::jit::Node *node) -> torch::jit::Operation {
        // How to find a graphRunner:
        // 1. See if a key based on graph hash has been registered
        // 2. If not, see if a key based on fusion node symbol string has been
        // registered, which is usually done in AOT fashion
        // 3. If not, create a graphRunner with graph hash as a key
        size_t key =
            reinterpret_cast<size_t>(node->g(at::attr::Subgraph)->block());
        std::string keyStr(sizeof(size_t), '\0');
        std::memcpy(&keyStr[0], &key, sizeof(key));
        std::shared_ptr<CachingGraphRunner> graphRunner =
            getGraphRunnerForKey(keyStr);

        if (!graphRunner) {
          graphRunner = getGraphRunnerForKey(node->kind().toQualString());
        }

        // If no preloaded graph runner was created for this node, create a new
        // empty one.
        if (!graphRunner) {
          graphRunner = setGraphRunnerForKey(keyStr, [node]() {
            return std::make_shared<CachingGraphRunner>(
                node->g(at::attr::Subgraph), getHostManager(),
                getBackendName().c_str(), getPyTorchLoaderSettings());
          });
        }

        return [graphRunner](torch::jit::Stack *stack) {
          Error err = Error::empty();
          // Store old Python signal handlers and install standard signal
          // handlers, so that it is possible to kill/interrupt the process if
          // needed.
          typedef void (*sighandler_t)(int);
          sighandler_t oldSigIntHandler = nullptr;
          sighandler_t oldSigTermHandler = nullptr;

          if (signalHandlerOverridesEnabled()) {
            oldSigIntHandler = signal(SIGINT, SIG_DFL);
            oldSigTermHandler = signal(SIGTERM, SIG_DFL);
          }

          if (graphRunner->getSettings().preCompilePyTorchModule) {
            err = graphRunner->runOnly(*stack);
          } else {
            err = graphRunner->run(*stack);
          }

          // Restore old signal handlers.
          if (oldSigIntHandler != nullptr && oldSigIntHandler != SIG_ERR &&
              oldSigIntHandler != SIG_DFL) {
            signal(SIGINT, oldSigIntHandler);
          }
          if (oldSigTermHandler != nullptr && oldSigTermHandler != SIG_ERR &&
              oldSigTermHandler != SIG_DFL) {
            signal(SIGTERM, oldSigTermHandler);
          }

          if (static_cast<bool>(err)) {
            // PyTorch framework expects an exception been thrown here.
            throw std::invalid_argument(ERR_TO_STRING(std::move(err)));
          }
        };
      },
      at::AliasAnalysisKind::PURE_FUNCTION)});
}

void registerGlowFusionPass(std::function<bool()> enablePassFn) {
  torch::jit::RegisterPass pass([enablePassFn = std::move(enablePassFn)](
                                    std::shared_ptr<torch::jit::Graph> &g) {
    if (enablePassFn()) {
      glow::glowCustomFuse(g, getGlowSymbol());
    }
  });
}

void registerGlowFusionOpAndPass(std::function<bool()> enablePassFn) {
  registerGlowOp(getGlowSymbol());
  registerGlowFusionPass(std::move(enablePassFn));
}
} // namespace glow
