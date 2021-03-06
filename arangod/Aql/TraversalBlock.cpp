////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "TraversalBlock.h"
#include "Aql/Ast.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionNode.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Functions.h"
#include "Basics/ScopeGuard.h"
#include "Cluster/ClusterTraverser.h"
#include "V8/v8-globals.h"
#include "V8Server/V8Traverser.h"

using namespace arangodb::aql;

using Json = arangodb::basics::Json;
using VertexId = arangodb::traverser::VertexId;

TraversalBlock::TraversalBlock(ExecutionEngine* engine, TraversalNode const* ep)
    : ExecutionBlock(engine, ep),
      _posInPaths(0),
      _useRegister(false),
      _usedConstant(false),
      _vertexReg(0),
      _edgeReg(0),
      _pathReg(0),
      _resolver(nullptr),
      _expressions(ep->expressions()),
      _hasV8Expression(false) {
  arangodb::traverser::TraverserOptions opts(_trx);
  ep->fillTraversalOptions(opts);
  auto ast = ep->_plan->getAst();

  for (auto& map : *_expressions) {
    for (size_t i = 0; i < map.second.size(); ++i) {
      SimpleTraverserExpression* it =
          dynamic_cast<SimpleTraverserExpression*>(map.second.at(i));
      if (it == nullptr) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                       std::string("invalid expression map"));
      }
      auto e = std::make_unique<Expression>(ast, it->compareToNode);
      _hasV8Expression |= e->isV8();
      std::unordered_set<Variable const*> inVars;
      e->variables(inVars);
      it->expression = e.release();

      // Prepare _inVars and _inRegs:
      _inVars.emplace_back();
      std::vector<Variable const*>& inVarsCur = _inVars.back();
      _inRegs.emplace_back();
      std::vector<RegisterId>& inRegsCur = _inRegs.back();

      for (auto const& v : inVars) {
        inVarsCur.emplace_back(v);
        auto it = ep->getRegisterPlan()->varInfo.find(v->id);
        TRI_ASSERT(it != ep->getRegisterPlan()->varInfo.end());
        TRI_ASSERT(it->second.registerId < ExecutionNode::MaxRegisterId);
        inRegsCur.emplace_back(it->second.registerId);
      }
    }
  }

  _resolver = new CollectionNameResolver(_trx->vocbase());

  if (arangodb::ServerState::instance()->isCoordinator()) {
    _traverser.reset(new arangodb::traverser::ClusterTraverser(
        ep->edgeColls(), opts,
        std::string(_trx->vocbase()->_name, strlen(_trx->vocbase()->_name)),
        _trx, _expressions));
  } else {
    _traverser.reset(
        new arangodb::traverser::DepthFirstTraverser(opts, _trx, _expressions));
  }
  if (!ep->usesInVariable()) {
    _vertexId = ep->getStartVertex();
  } else {
    auto it = ep->getRegisterPlan()->varInfo.find(ep->inVariable()->id);
    TRI_ASSERT(it != ep->getRegisterPlan()->varInfo.end());
    _reg = it->second.registerId;
    _useRegister = true;
  }
  _vertexVar = nullptr;
  _edgeVar = nullptr;
  _pathVar = nullptr;
  if (ep->usesVertexOutVariable()) {
    _vertexVar = ep->vertexOutVariable();
  }

  if (ep->usesEdgeOutVariable()) {
    _edgeVar = ep->edgeOutVariable();
  }

  if (ep->usesPathOutVariable()) {
    _pathVar = ep->pathOutVariable();
  }
}

TraversalBlock::~TraversalBlock() {
  delete _resolver;
  freeCaches();
}

void TraversalBlock::freeCaches() {
  for (auto& v : _vertices) {
    v.destroy();
  }
  _vertices.clear();
  for (auto& e : _edges) {
    e.destroy();
  }
  _edges.clear();
  for (auto& p : _paths) {
    p.destroy();
  }
  _paths.clear();
}

int TraversalBlock::initialize() {
  DEBUG_BEGIN_BLOCK();
  int res = ExecutionBlock::initialize();
  auto varInfo = getPlanNode()->getRegisterPlan()->varInfo;

  if (usesVertexOutput()) {
    TRI_ASSERT(_vertexVar != nullptr);
    auto it = varInfo.find(_vertexVar->id);
    TRI_ASSERT(it != varInfo.end());
    TRI_ASSERT(it->second.registerId < ExecutionNode::MaxRegisterId);
    _vertexReg = it->second.registerId;
  }
  if (usesEdgeOutput()) {
    TRI_ASSERT(_edgeVar != nullptr);
    auto it = varInfo.find(_edgeVar->id);
    TRI_ASSERT(it != varInfo.end());
    TRI_ASSERT(it->second.registerId < ExecutionNode::MaxRegisterId);
    _edgeReg = it->second.registerId;
  }
  if (usesPathOutput()) {
    TRI_ASSERT(_pathVar != nullptr);
    auto it = varInfo.find(_pathVar->id);
    TRI_ASSERT(it != varInfo.end());
    TRI_ASSERT(it->second.registerId < ExecutionNode::MaxRegisterId);
    _pathReg = it->second.registerId;
  }

  return res;
  DEBUG_END_BLOCK();
}

void TraversalBlock::executeExpressions() {
  DEBUG_BEGIN_BLOCK();
  AqlItemBlock* cur = _buffer.front();
  for (auto& map : *_expressions) {
    for (size_t i = 0; i < map.second.size(); ++i) {
      // Right now no inVars are allowed.
      SimpleTraverserExpression* it =
          dynamic_cast<SimpleTraverserExpression*>(map.second.at(i));
      if (it != nullptr && it->expression != nullptr) {
        // inVars and inRegs needs fixx
        bool mustDestroy;
        AqlValue a = it->expression->execute(_trx, cur, _pos, _inVars[i],
                                             _inRegs[i], mustDestroy);

        AqlValueGuard guard(a, mustDestroy);
        
        AqlValueMaterializer materializer(_trx);
        VPackSlice slice = materializer.slice(a, false);

        VPackBuilder* builder = new VPackBuilder;
        try {
          builder->add(slice);
        } catch (...) {
          delete builder;
          throw;
        }

        it->compareTo.reset(builder);
      }
    }
  }
  throwIfKilled();  // check if we were aborted
  DEBUG_END_BLOCK();
}

void TraversalBlock::executeFilterExpressions() {
  DEBUG_BEGIN_BLOCK();
  if (!_expressions->empty()) {
    if (_hasV8Expression) {
      bool const isRunningInCluster =
          arangodb::ServerState::instance()->isRunningInCluster();

      // must have a V8 context here to protect Expression::execute()
      auto engine = _engine;
      arangodb::basics::ScopeGuard guard{
          [&engine]() -> void { engine->getQuery()->enterContext(); },
          [&]() -> void {
            if (isRunningInCluster) {
              // must invalidate the expression now as we might be called from
              // different threads
              for (auto const& map : *_expressions) {
                for (auto const& e : map.second) {
                  auto casted = dynamic_cast<SimpleTraverserExpression*>(e);
                  if (casted != nullptr) {
                    casted->expression->invalidate();
                  }
                }
              }

              engine->getQuery()->exitContext();
            }
          }};

      ISOLATE;
      v8::HandleScope scope(isolate);  // do not delete this!

      executeExpressions();
      TRI_IF_FAILURE("TraversalBlock::executeV8") {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
      }
    } else {
      // no V8 context required!

      Functions::InitializeThreadContext();
      try {
        executeExpressions();
        TRI_IF_FAILURE("TraversalBlock::executeExpression") {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
        }
        Functions::DestroyThreadContext();
      } catch (...) {
        Functions::DestroyThreadContext();
        throw;
      }
    }
  }
  DEBUG_END_BLOCK();
}

int TraversalBlock::initializeCursor(AqlItemBlock* items, size_t pos) {
  _pos = 0;
  _posInPaths = 0;
  _usedConstant = false;
  freeCaches();
  return ExecutionBlock::initializeCursor(items, pos);
}

/// @brief read more paths
bool TraversalBlock::morePaths(size_t hint) {
  DEBUG_BEGIN_BLOCK();
  freeCaches();
  _posInPaths = 0;
  if (!_traverser->hasMore()) {
    _engine->_stats.scannedIndex += _traverser->getAndResetReadDocuments();
    _engine->_stats.filtered += _traverser->getAndResetFilteredPaths();
    return false;
  }

  VPackBuilder tmp;
  for (size_t j = 0; j < hint; ++j) {
    std::unique_ptr<arangodb::traverser::TraversalPath> p(_traverser->next());

    if (p == nullptr) {
      // There are no further paths available.
      break;
    }

    if (usesVertexOutput()) {
      tmp.clear();
      p->lastVertexToVelocyPack(_trx, tmp);
      _vertices.emplace_back(tmp.slice());
    }
    if (usesEdgeOutput()) {
      tmp.clear();
      p->lastEdgeToVelocyPack(_trx, tmp);
      _edges.emplace_back(tmp.slice());
    }
    if (usesPathOutput()) {
      tmp.clear();
      p->pathToVelocyPack(_trx, tmp);
      _paths.emplace_back(tmp.slice());
    }
    _engine->_stats.scannedIndex += p->getReadDocuments();

    throwIfKilled();  // check if we were aborted
  }

  _engine->_stats.scannedIndex += _traverser->getAndResetReadDocuments();
  _engine->_stats.filtered += _traverser->getAndResetFilteredPaths();
  return !_vertices.empty();
  DEBUG_END_BLOCK();
}

/// @brief skip the next paths
size_t TraversalBlock::skipPaths(size_t hint) {
  DEBUG_BEGIN_BLOCK();
  freeCaches();
  _posInPaths = 0;
  if (!_traverser->hasMore()) {
    return 0;
  }
  return _traverser->skip(hint);
  DEBUG_END_BLOCK();
}

/// @brief initialize the list of paths
void TraversalBlock::initializePaths(AqlItemBlock const* items) {
  DEBUG_BEGIN_BLOCK();
  if (!_vertices.empty()) {
    // No Initialization required.
    return;
  }
  if (!_useRegister) {
    if (!_usedConstant) {
      _usedConstant = true;
      auto pos = _vertexId.find("/");
      if (pos == std::string::npos) {
        _engine->getQuery()->registerWarning(TRI_ERROR_BAD_PARAMETER,
                                             "Invalid input for traversal: "
                                             "Only id strings or objects with "
                                             "_id are allowed");
      } else {
        _traverser->setStartVertex(_vertexId);
      }
    }
  } else {
    AqlValue const& in = items->getValueReference(_pos, _reg);
    if (in.isObject()) {
      try {
        std::string idString = _trx->extractIdString(in.slice());
        _traverser->setStartVertex(idString);
      }
      catch (...) {
        // _id or _key not present... ignore this error and fall through
      }
    } else if (in.isString()) {
      _vertexId = in.slice().copyString();
      _traverser->setStartVertex(_vertexId);
    } else {
      _engine->getQuery()->registerWarning(TRI_ERROR_BAD_PARAMETER,
                                           "Invalid input for traversal: Only "
                                           "id strings or objects with _id are "
                                           "allowed");
    }
  }
  DEBUG_END_BLOCK();
}

/// @brief getSome
AqlItemBlock* TraversalBlock::getSome(size_t,  // atLeast,
                                      size_t atMost) {
  DEBUG_BEGIN_BLOCK();
  if (_done) {
    return nullptr;
  }

  if (_buffer.empty()) {
    size_t toFetch = (std::min)(DefaultBatchSize(), atMost);
    if (!ExecutionBlock::getBlock(toFetch, toFetch)) {
      _done = true;
      return nullptr;
    }
    _pos = 0;  // this is in the first block
    executeFilterExpressions();
  }

  // If we get here, we do have _buffer.front()
  AqlItemBlock* cur = _buffer.front();
  size_t const curRegs = cur->getNrRegs();

  if (_pos == 0) {
    // Initial initialisation
    initializePaths(cur);
  }

  // Iterate more paths:
  if (_posInPaths >= _vertices.size()) {
    if (!morePaths(atMost)) {
      // This input does not have any more paths. maybe the next one has.
      // we can only return nullptr iff the buffer is empty.
      if (++_pos >= cur->size()) {
        _buffer.pop_front();  // does not throw
        // returnBlock(cur);
        delete cur;
        _pos = 0;
      } else {
        initializePaths(cur);
      }
      return getSome(atMost, atMost);
    }
  }

  size_t available = _vertices.size() - _posInPaths;
  size_t toSend = (std::min)(atMost, available);

  RegisterId nrRegs =
      getPlanNode()->getRegisterPlan()->nrRegs[getPlanNode()->getDepth()];

  std::unique_ptr<AqlItemBlock> res(requestBlock(toSend, nrRegs));
  // automatically freed if we throw
  TRI_ASSERT(curRegs <= res->getNrRegs());

  // only copy 1st row of registers inherited from previous frame(s)
  inheritRegisters(cur, res.get(), _pos);

  for (size_t j = 0; j < toSend; j++) {
    if (j > 0) {
      // re-use already copied aqlvalues
      for (RegisterId i = 0; i < curRegs; i++) {
        res->setValue(j, i, res->getValueReference(0, i));
        // Note: if this throws, then all values will be deleted
        // properly since the first one is.
      }
    }
    if (usesVertexOutput()) {
      res->setValue(j, _vertexReg, _vertices[_posInPaths].clone());
    }
    if (usesEdgeOutput()) {
      res->setValue(j, _edgeReg, _edges[_posInPaths].clone());
    }
    if (usesPathOutput()) {
      res->setValue(j, _pathReg, _paths[_posInPaths].clone());
    }
    ++_posInPaths;
  }
  // Advance read position:
  if (_posInPaths >= _vertices.size()) {
    // we have exhausted our local paths buffer
    // fetch more paths into our buffer
    if (!morePaths(atMost)) {
      // nothing more to read, re-initialize fetching of paths
      if (++_pos >= cur->size()) {
        _buffer.pop_front();  // does not throw
        // returnBlock(cur);
        delete cur;
        _pos = 0;
      } else {
        initializePaths(cur);
      }
    }
  }

  // Clear out registers no longer needed later:
  clearRegisters(res.get());
  return res.release();
  DEBUG_END_BLOCK();
}

/// @brief skipSome
size_t TraversalBlock::skipSome(size_t atLeast, size_t atMost) {
  DEBUG_BEGIN_BLOCK();
  size_t skipped = 0;

  if (_done) {
    return skipped;
  }

  if (_buffer.empty()) {
    size_t toFetch = (std::min)(DefaultBatchSize(), atMost);
    if (!ExecutionBlock::getBlock(toFetch, toFetch)) {
      _done = true;
      return skipped;
    }
    _pos = 0;  // this is in the first block
    executeFilterExpressions();
  }

  // If we get here, we do have _buffer.front()
  AqlItemBlock* cur = _buffer.front();

  if (_pos == 0) {
    // Initial initialisation
    initializePaths(cur);
  }

  size_t available = _vertices.size() - _posInPaths;
  // We have not yet fetched any paths. We can skip the next atMost many
  if (available == 0) {
    return skipPaths(atMost);
  }
  // We have fewer paths available in our list, so we clear the list and thereby
  // skip these.
  if (available <= atMost) {
    freeCaches();
    _posInPaths = 0;
    return available;
  }
  _posInPaths += atMost;
  // Skip the next atMost many paths.
  return atMost;
  DEBUG_END_BLOCK();
}
