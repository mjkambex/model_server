//*****************************************************************************
// Copyright 2020 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************
#include "pipelinedefinition.hpp"

#include <chrono>
#include <set>
#include <thread>

#include "pipelinedefinitionunloadguard.hpp"
#include "prediction_service_utils.hpp"

namespace ovms {

Status toNodeKind(const std::string& str, NodeKind& nodeKind) {
    if (str == DL_NODE_CONFIG_TYPE) {
        nodeKind = NodeKind::DL;
        return StatusCode::OK;
    }
    SPDLOG_ERROR("Unsupported node type:{}", str);
    return StatusCode::PIPELINE_NODE_WRONG_KIND_CONFIGURATION;
}

Status PipelineDefinition::validate(ModelManager& manager) {
    struct ValidationResultNotifier {
        ValidationResultNotifier() {}
        ~ValidationResultNotifier() {
            if (passed) {
                // status.notifyValidationPassed();
            } else {
                // status.notifyValidationFailed();
            }
        }
        bool passed = false;
    };
    ValidationResultNotifier notifier;
    Status validationResult = validateNodes(manager);
    if (!validationResult.ok()) {
        return validationResult;
    }

    validationResult = validateForCycles();
    if (!validationResult.ok()) {
        return validationResult;
    }
    notifier.passed = true;
    return validationResult;
}

Status PipelineDefinition::reload(ModelManager& manager, const std::vector<NodeInfo>&& nodeInfos, const pipeline_connections_t&& connections) {
    resetSubscriptions(manager);
    // this->status.notifyLoadInProgress();
    while (requestsHandlesCounter > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(1));
    }

    this->nodeInfos = std::move(nodeInfos);
    this->connections = std::move(connections);
    makeSubscriptions(manager);

    return validate(manager);
}

void PipelineDefinition::retire(ModelManager& manager) {
    resetSubscriptions(manager);
    // this->status.notifyRetire();
    while (requestsHandlesCounter > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
    this->nodeInfos.clear();
    this->connections.clear();
}

Status PipelineDefinition::waitForLoaded(std::unique_ptr<PipelineDefinitionUnloadGuard>& unloadGuard, const uint waitForLoadedTimeoutMicroseconds) {
    unloadGuard = std::make_unique<PipelineDefinitionUnloadGuard>(*this);
    /*
    const uint waitLoadedTimestepMicroseconds = 10;
    const uint waitCheckpoints = waitForLoadedTimeoutMicroseconds / waitLoadedTimestepMicroseconds;
    uint waitCheckpointsCounter = waitCheckpoints;
    while (waitCheckpointsCounter-- != 0) {
        if (status.getState() == ModelVersionState::AVAILABLE) {
            SPDLOG_INFO("Succesfully waited for pipeline:{}", getName()); // TODO change to DEBUG after part 2
            break;
        }
        unloadGuard.reset();
        if (status.getState() > ModelVersionState::AVAILABLE) {
            SPDLOG_INFO("Waiting for pipeline:{} ended since it started unloading.", getName()); // TODO change to DEBUG after part 2
            return StatusCode::MODEL_VERSION_NOT_LOADED_ANYMORE;
        }
        SPDLOG_INFO("Waiting for available state for pipeline:{}, with timestep:{} timeout:{} check count:{}",
                getName(), waitLoadedTimestepMicroseconds, waitForModelLoadedTimeoutMicroseconds, waitCheckpointsCounter); // TODO change to DEBUG after part 2 finished
        status.waitForLoadedNotify(waitLoadedTimestepMicroseconds);
        unloadGuard = std::make_unique<PipelineDefinitionUnloadGuard(*this);
    }
    if (status.getState() != ModelVersionState::AVAILABLE) {
        if (status.getState() > ModelVersionState::AVAILABLE) {
            SPDLOG_INFO("Waiting for pipeline:{} ended since it started unloading.", getName()); // TODO change to DEBUG after part 2
            return StatusCode::MODEL_VERSION_NOT_LOADED_ANYMORE;
        } else if (status.getState() > ModelVersionState::AVAILABLE) {
            SPDLOG_INFO("Waiting for pipeline:{} ended due to timeout.", getName()); // TODO change to DEBUG after part 2
            return StatusCode::MODEL_VERSION_NOT_LOADED_YET;
        }
    }
    */
    return StatusCode::OK;
}

Status PipelineDefinition::create(std::unique_ptr<Pipeline>& pipeline,
    const tensorflow::serving::PredictRequest* request,
    tensorflow::serving::PredictResponse* response,
    ModelManager& manager) {
    std::unique_ptr<PipelineDefinitionUnloadGuard> unloadGuard;
    Status status = waitForLoaded(unloadGuard);
    if (!status.ok()) {
        return status;
    }

    std::unordered_map<std::string, std::unique_ptr<Node>> nodes;
    EntryNode* entry = nullptr;
    ExitNode* exit = nullptr;
    for (const auto& info : nodeInfos) {
        SPDLOG_DEBUG("Creating pipeline:{}. Adding nodeName:{}, modelName:{}",
            getName(), info.nodeName, info.modelName);
        switch (info.kind) {
        case NodeKind::ENTRY: {
            auto node = std::make_unique<EntryNode>(request);
            entry = node.get();
            nodes.insert(std::make_pair(info.nodeName, std::move(node)));
            break;
        }
        case NodeKind::DL:
            nodes.insert(std::make_pair(info.nodeName, std::move(std::make_unique<DLNode>(info.nodeName,
                                                           info.modelName,
                                                           info.modelVersion,
                                                           manager,
                                                           info.outputNameAliases))));
            break;
        case NodeKind::EXIT: {
            auto node = std::make_unique<ExitNode>(response);
            exit = node.get();
            nodes.insert(std::make_pair(info.nodeName, std::move(node)));
            break;
        }
        default:
            throw std::invalid_argument("unknown node kind");
        }
    }
    for (const auto& kv : connections) {
        const auto& dependantNode = nodes.at(kv.first);
        for (const auto& pair : kv.second) {
            const auto& dependencyNode = nodes.at(pair.first);
            SPDLOG_DEBUG("Connecting pipeline:{}, from:{}, to:{}", getName(), dependencyNode->getName(), dependantNode->getName());
            Pipeline::connect(*dependencyNode, *dependantNode, pair.second);
        }
    }
    pipeline = std::make_unique<Pipeline>(*entry, *exit, pipelineName);
    for (auto& kv : nodes) {
        pipeline->push(std::move(kv.second));
    }
    return status;
}

void PipelineDefinition::resetSubscriptions(ModelManager& manager) {
    for (auto& [modelName, modelVersion] : subscriptions) {
        if (modelVersion) {
            SPDLOG_INFO("Unsubscribing pipeline:{} from model: {}, version:{}",
                getName(), modelName, modelVersion);
            manager.findModelByName(modelName)->getModelInstanceByVersion(modelVersion)->unsubscribe(*this);
        } else {  // using default version
            SPDLOG_INFO("Unsubscribing pipeline:{} from model: {}",
                getName(), modelName);
            manager.findModelByName(modelName)->unsubscribe(*this);
        }
    }
    subscriptions.clear();
}

static std::string createSubscriptionErrorMessage(const std::string& pipelineName, const NodeInfo& nodeInfo) {
    std::stringstream ss;
    ss << "Pipeline: " << pipelineName << " Failed to make subscription to model: " << nodeInfo.modelName;
    if (nodeInfo.modelVersion) {
        ss << " version: " << nodeInfo.modelVersion.value();
    }
    ss << " because it was missing";
    return ss.str();
}

void PipelineDefinition::makeSubscriptions(ModelManager& manager) {
    for (auto& node : nodeInfos) {
        if (node.kind == NodeKind::DL) {
            if (subscriptions.find({node.modelName, node.modelVersion.value_or(0)}) != subscriptions.end()) {
                continue;
            }
            auto model = manager.findModelByName(node.modelName);
            if (nullptr == model) {
                SPDLOG_WARN(createSubscriptionErrorMessage(getName(), node));
                continue;
            }
            if (node.modelVersion) {
                auto modelInstance = model->getModelInstanceByVersion(node.modelVersion.value());
                if (nullptr == modelInstance) {
                    SPDLOG_WARN(createSubscriptionErrorMessage(getName(), node));
                    continue;
                }
                modelInstance->subscribe(*this);
            } else {
                model->subscribe(*this);
            }
            subscriptions.insert({node.modelName, node.modelVersion.value_or(0)});
        }
    }
}

Status PipelineDefinition::validateNode(ModelManager& manager, const NodeInfo& dependantNodeInfo) {
    std::unique_ptr<ModelInstanceUnloadGuard> dependantModelUnloadGuard;
    std::shared_ptr<ModelInstance> dependantModelInstance;
    std::set<std::string> remainingUnconnectedDependantModelInputs;
    SPDLOG_DEBUG("Validation of pipeline: {}; node name: {}; node kind: {}",
        getName(),
        dependantNodeInfo.nodeName,
        dependantNodeInfo.kind);

    // If we validate DL model node, retrieve underlying model instance.
    if (dependantNodeInfo.kind == NodeKind::DL) {
        if (!getModelInstance(
                manager,
                dependantNodeInfo.modelName,
                dependantNodeInfo.modelVersion.value_or(0),
                dependantModelInstance,
                dependantModelUnloadGuard)
                 .ok()) {
            SPDLOG_ERROR("Validation of pipeline({}) definition failed. Missing model: {}; version: {}",
                this->pipelineName,
                dependantNodeInfo.modelName,
                dependantNodeInfo.modelVersion.value_or(0));
            return StatusCode::PIPELINE_NODE_REFERING_TO_MISSING_MODEL;  // REACHED
        }

        // Ban creating pipelines with Dynamic Model Parameters.
        const auto& config = dependantModelInstance->getModelConfig();
        if (config.getBatchingMode() == Mode::AUTO) {
            SPDLOG_ERROR("Validation of pipeline({}) definition failed. Node name {} used model name {} with dynamic batch size which is forbidden.",
                this->pipelineName,
                dependantNodeInfo.nodeName,
                dependantNodeInfo.modelName);
            return StatusCode::FORBIDDEN_MODEL_DYNAMIC_PARAMETER;  // REACHED
        }

        for (const auto& [name, info] : config.getShapes()) {
            if (info.shapeMode == Mode::AUTO) {
                SPDLOG_ERROR("Validation of pipeline({}) definition failed. Node name {} used model name {} with dynamic shape which is forbidden.",
                    this->pipelineName,
                    dependantNodeInfo.nodeName,
                    dependantNodeInfo.modelName);
                return StatusCode::FORBIDDEN_MODEL_DYNAMIC_PARAMETER;  // REACHED
            }
        }

        // Save set of inputs which are required by underlying model of currently validated node.
        // This is later used to make sure we feed each input exactly one data source.
        std::transform(
            dependantModelInstance->getInputsInfo().begin(),
            dependantModelInstance->getInputsInfo().end(),
            std::inserter(
                remainingUnconnectedDependantModelInputs,
                remainingUnconnectedDependantModelInputs.end()),
            [](auto pair) { return pair.first; });
    }

    // Check all connections entering currently validated node.
    for (const auto& [dependencyNodeName, mapping] : connections[dependantNodeInfo.nodeName]) {
        // This check needs to be performed here instead of at the beginning of method.
        // This is because we allow adding connection elements with no input pairs specified.
        // TODO: Try to ban size 0 of mapping? PIPELINE_DEFINITION_MISSING_DEPENDENCY_MAPPING
        if (dependantNodeInfo.kind == NodeKind::ENTRY) {
            if (mapping.size() > 0) {
                return StatusCode::UNKNOWN_ERROR;
            } else {
                continue;
            }
        }

        // Find dependency node info object.
        auto dependencyNodeInfo = std::find_if(
            std::begin(this->nodeInfos),
            std::end(this->nodeInfos),
            [dependencyNodeName](const NodeInfo& nodeInfo) { return nodeInfo.nodeName == dependencyNodeName; });
        if (dependencyNodeInfo == std::end(this->nodeInfos)) {
            SPDLOG_ERROR("Validation of pipeline({}) definition failed. Node (name:{}) is connected to missing dependency node (name:{})",
                this->pipelineName,
                dependantNodeInfo.nodeName,
                dependencyNodeName);
            return StatusCode::PIPELINE_NODE_REFERING_TO_MISSING_NODE;  // REACHED
        }

        // Exit cannot be dependency of any node.
        if (dependencyNodeInfo->kind == NodeKind::EXIT) {
            return StatusCode::UNKNOWN_ERROR;  // REACHED
        }

        // At this point dependency node can only be either DL model node or entry node.
        // Take care when adding new node types.
        // If underlying model is of type DL model, retrieve underlying model instance for later use.
        std::unique_ptr<ModelInstanceUnloadGuard> dependencyModelUnloadGuard;
        std::shared_ptr<ModelInstance> dependencyModelInstance;
        if (dependencyNodeInfo->kind == NodeKind::DL) {
            if (!getModelInstance(
                    manager,
                    dependencyNodeInfo->modelName,
                    dependencyNodeInfo->modelVersion.value_or(0),
                    dependencyModelInstance,
                    dependencyModelUnloadGuard)
                     .ok()) {
                SPDLOG_ERROR("Validation of pipeline({}) definition failed. Dependency DL model node refers to unavailable model - name:{}; version:{}",
                    this->pipelineName,
                    dependencyNodeInfo->modelName,
                    dependencyNodeInfo->modelVersion.value_or(0));
                return StatusCode::PIPELINE_NODE_REFERING_TO_MISSING_MODEL;
            }
        }

        // Validate each connection between dependency and dependant node.
        for (const auto& [alias, realName] : mapping) {
            // If currently validated node is of type DL model, mark its input as connected
            // by erasing from previously gathered input set.
            // If such input cannot be found in the map, it means we refer
            // to non existing model input or we already connected it to some other data source which is invalid.
            if (dependantNodeInfo.kind == NodeKind::DL) {
                if (remainingUnconnectedDependantModelInputs.erase(realName) == 0) {
                    SPDLOG_ERROR("Validation of pipeline({}) definition failed. Node:{} model:{} version:{} has no input with name:{}",
                        this->pipelineName,
                        dependantNodeInfo.nodeName,
                        dependantNodeInfo.modelName,
                        dependantNodeInfo.modelVersion.value_or(0),
                        realName);
                    return StatusCode::PIPELINE_CONNECTION_TO_MISSING_NODE_INPUT;  // REACHED
                }
            }

            // Check whether node is configured to have such output.
            if (dependencyNodeInfo->outputNameAliases.count(alias) == 0) {
                SPDLOG_ERROR("Validation of pipeline({}) definition failed. Missing dependency node:{} data item:{} for dependant node:{}",
                    this->pipelineName,
                    dependencyNodeInfo->nodeName,
                    alias,
                    dependantNodeInfo.nodeName);
                return StatusCode::PIPELINE_NODE_REFERING_TO_MISSING_DATA_SOURCE;  // REACHED
            }

            // If dependency node is of type DL model, make sure there is underlying model output present.
            if (dependencyNodeInfo->kind == NodeKind::DL) {
                // Check whether underlying model contains required output.
                const auto& modelOutputName = dependencyNodeInfo->outputNameAliases.at(alias);
                if (dependencyModelInstance->getOutputsInfo().count(modelOutputName) == 0) {
                    SPDLOG_ERROR("Validation of pipeline({}) definition failed. Missing model (name:{}, version:{}) output:{} of dependency node:{}",
                        this->pipelineName,
                        dependencyNodeInfo->modelName,
                        dependencyNodeInfo->modelVersion.value_or(0),
                        modelOutputName,
                        dependencyNodeInfo->nodeName);
                    return StatusCode::PIPELINE_NODE_REFERING_TO_MISSING_MODEL_OUTPUT;  // REACHED
                }
            }

            // If we connection we validate is refering to data source from gRPC/REST request,
            // make sure we defined such input names in pipeline input field.
            if (dependantNodeInfo.kind == NodeKind::DL &&
                dependencyNodeInfo->kind == NodeKind::ENTRY) {
                const auto& pipelineInputName = alias;
                if (dependencyNodeInfo->outputNameAliases.count(pipelineInputName) == 0) {
                    SPDLOG_ERROR("Validation of pipeline({}) definition failed. Missing pipeline input:{} for dependant node:{}",
                        this->pipelineName,
                        pipelineInputName,
                        dependantNodeInfo.nodeName);
                    return StatusCode::PIPELINE_NODE_REFERING_TO_MISSING_DATA_SOURCE;  // REACHED
                }
            }

            // If validated connection pair connects two DL model nodes,
            // check if both input/output exist and its metadata (shape, precision) matches.
            if (dependantNodeInfo.kind == NodeKind::DL &&
                dependencyNodeInfo->kind == NodeKind::DL) {
                const auto& modelOutputName = dependencyNodeInfo->outputNameAliases.at(alias);
                const auto& modelInputName = realName;
                const auto& tensorInput = dependantModelInstance->getInputsInfo().at(modelInputName);
                const auto& tensorOutput = dependencyModelInstance->getOutputsInfo().at(modelOutputName);
                if (tensorInput->getShape() != tensorOutput->getShape()) {
                    SPDLOG_ERROR("Validation of pipeline({}) definition failed. Shape mismatch between: dependant node:{}; model:{}; version:{}; input:{}; shape:{} vs dependency node:{}; model:{}; version:{}; output:{}; shape:{}",
                        this->pipelineName,
                        dependantNodeInfo.nodeName,
                        dependantNodeInfo.modelName,
                        dependantNodeInfo.modelVersion.value_or(0),
                        modelInputName,
                        TensorInfo::shapeToString(tensorInput->getShape()),
                        dependencyNodeInfo->nodeName,
                        dependencyNodeInfo->modelName,
                        dependencyNodeInfo->modelVersion.value_or(0),
                        modelOutputName,
                        TensorInfo::shapeToString(tensorOutput->getShape()));
                    return StatusCode::INVALID_SHAPE;  // REACHED
                }
                if (tensorInput->getPrecision() != tensorOutput->getPrecision()) {
                    SPDLOG_ERROR("Validation of pipeline({}) definition failed. Precision mismatch between: dependant node:{}; model:{}; version:{}; input:{}; precision:{} vs dependency node:{}; model:{}; version:{}; output:{}; precision:{}",
                        this->pipelineName,
                        dependantNodeInfo.nodeName,
                        dependantNodeInfo.modelName,
                        dependantNodeInfo.modelVersion.value_or(0),
                        modelInputName,
                        tensorInput->getPrecisionAsString(),
                        dependencyNodeInfo->nodeName,
                        dependencyNodeInfo->modelName,
                        dependencyNodeInfo->modelVersion.value_or(0),
                        modelOutputName,
                        tensorOutput->getPrecisionAsString());
                    return StatusCode::INVALID_PRECISION;  // CANNOT REACH MANUALLY, WE HAVE NO OTHER PRECISION SUPPORT
                }
            }
        }
    }

    // Make sure all model inputs of validated node is fed with some data source.
    if (remainingUnconnectedDependantModelInputs.size() > 0) {
        std::stringstream ss;
        for (const auto& input : remainingUnconnectedDependantModelInputs) {
            ss << input << ", ";
        }
        SPDLOG_ERROR("Validation of pipeline({}) definition failed. Node:{} model:{} version:{} has inputs:({}) not connected to any source",
            this->pipelineName,
            dependantNodeInfo.nodeName,
            dependantNodeInfo.modelName,
            dependantNodeInfo.modelVersion.value_or(0),
            ss.str());
        return StatusCode::PIPELINE_NOT_ALL_INPUTS_CONNECTED;  // REACHED
    }

    return StatusCode::OK;
}

// Because of the way how pipeline_connections is implemented, this function is using
// transpose of PipelineDefinition graph.(Transpose contains same cycles as original graph)
Status PipelineDefinition::validateForCycles() {
    std::vector<std::string> visited;
    std::vector<std::string> parentNodes;
    visited.reserve(nodeInfos.size());
    parentNodes.reserve(nodeInfos.size());

    auto pred = [](const NodeInfo& nodeInfo) {
        return nodeInfo.kind == NodeKind::EXIT;
    };

    const auto& itr = std::find_if(std::begin(nodeInfos), std::end(nodeInfos), pred);
    if (itr == nodeInfos.end()) {
        SPDLOG_ERROR("Pipeline does not contain response node.");
        return StatusCode::PIPELINE_MISSING_ENTRY_OR_EXIT;
    }
    std::string nodeName = itr->nodeName;
    visited.push_back(nodeName);

    bool anyUnvisitedLeft = true;
    while (anyUnvisitedLeft) {
        bool unvisistedFound = false;
        const auto& connectedToNode = connections[nodeName];
        for (const auto& node : connectedToNode) {
            if (nodeName == node.first) {
                SPDLOG_ERROR("Node {} is connected to itself.", nodeName);
                return StatusCode::PIPELINE_CYCLE_FOUND;
            }

            if (std::find(visited.begin(), visited.end(), node.first) == visited.end()) {
                parentNodes.push_back(nodeName);
                visited.push_back(node.first);
                nodeName = node.first;
                unvisistedFound = true;
                break;
            } else {
                if (std::find(parentNodes.begin(), parentNodes.end(), node.first) != parentNodes.end()) {
                    std::string cycleNodes;
                    for (auto& cycleNode : parentNodes) {
                        cycleNodes += cycleNode;
                        if (cycleNode != parentNodes.back()) {
                            cycleNodes += ", ";
                        }
                    }
                    SPDLOG_ERROR("Following nodes creates cycle: {}", cycleNodes);
                    return StatusCode::PIPELINE_CYCLE_FOUND;
                }
            }
        }

        if (!unvisistedFound) {
            if (parentNodes.size() == 0) {
                anyUnvisitedLeft = false;
                if (visited.size() != nodeInfos.size()) {
                    SPDLOG_ERROR("There are nodes not connected to pipeline.");
                    return StatusCode::PIPELINE_CONTAINS_UNCONNECTED_NODES;
                }
            } else {
                nodeName = parentNodes.back();
                parentNodes.pop_back();
            }
        }
    }
    return StatusCode::OK;
}

Status PipelineDefinition::validateNodes(ModelManager& manager) {
    SPDLOG_DEBUG("Validation of pipeline definition:{} nodes started.", getName());

    int entryNodeCount = std::count_if(
        this->nodeInfos.begin(),
        this->nodeInfos.end(),
        [](const NodeInfo& info) { return info.kind == NodeKind::ENTRY; });

    int exitNodeCount = std::count_if(
        this->nodeInfos.begin(),
        this->nodeInfos.end(),
        [](const NodeInfo& info) { return info.kind == NodeKind::EXIT; });

    if (entryNodeCount <= 0) {
        SPDLOG_ERROR("PipelineDefinition: {} is missing request node", pipelineName);
        return StatusCode::PIPELINE_MISSING_ENTRY_OR_EXIT;
    }

    if (exitNodeCount <= 0) {
        SPDLOG_ERROR("PipelineDefinition: {} is missing response node", pipelineName);
        return StatusCode::PIPELINE_MISSING_ENTRY_OR_EXIT;
    }

    if (entryNodeCount > 1) {
        SPDLOG_ERROR("PipelineDefinition: {} has multiple request nodes", pipelineName);
        return StatusCode::PIPELINE_MULTIPLE_ENTRY_NODES;
    }

    if (exitNodeCount > 1) {
        SPDLOG_ERROR("PipelineDefinition: {} has multiple response nodes", pipelineName);
        return StatusCode::PIPELINE_MULTIPLE_EXIT_NODES;
    }

    for (const auto& node : nodeInfos) {
        auto findByName = [node](const NodeInfo& nodeInfo) {
            return nodeInfo.nodeName == node.nodeName;
        };

        if (std::count_if(nodeInfos.begin(), nodeInfos.end(), findByName) > 1) {
            SPDLOG_ERROR("PipelineDefinition: {} has multiple nodes with name {}", pipelineName, node.nodeName);
            return StatusCode::PIPELINE_NODE_NAME_DUPLICATE;
        }

        auto result = validateNode(manager, node);
        if (!result.ok()) {
            return result;
        }
    }

    return StatusCode::OK;
}

Status PipelineDefinition::getInputsInfo(tensor_map_t& inputsInfo, const ModelManager& manager) const {
    // Assumptions: this can only be called on available pipeline definition.
    // Add check if available when pipeline status will be implemented.

    static const auto byName = [](const std::string& name) {
        return [name](const NodeInfo& nodeInfo) {
            return nodeInfo.nodeName == name;
        };
    };

    for (const auto& [dependantNodeName, allMappings] : connections) {
        const auto& dependantNodeInfo = std::find_if(std::begin(nodeInfos), std::end(nodeInfos), byName(dependantNodeName));
        for (const auto& [dependencyNodeName, specificDependencyMapping] : allMappings) {
            const auto& dependencyNodeInfo = std::find_if(std::begin(nodeInfos), std::end(nodeInfos), byName(dependencyNodeName));
            if (dependencyNodeInfo->kind != NodeKind::ENTRY) {
                continue;
            }

            switch (dependantNodeInfo->kind) {
            case NodeKind::EXIT: {
                for (const auto& [alias, realName] : specificDependencyMapping) {
                    inputsInfo.insert({alias, TensorInfo::getUnspecifiedTensorInfo()});
                }
                break;
            }
            case NodeKind::DL: {
                auto instance = manager.findModelInstance(dependantNodeInfo->modelName, dependantNodeInfo->modelVersion.value_or(0));
                if (!instance) {
                    // TODO: Change to SPDLOG_DEBUG before release
                    SPDLOG_INFO("Model:{} was unavailable during pipeline:{} inputs info fetching", dependantNodeInfo->modelName, this->getName());
                    return StatusCode::MODEL_MISSING;
                }
                std::unique_ptr<ModelInstanceUnloadGuard> unloadGuard;
                auto status = instance->waitForLoaded(0, unloadGuard);
                if (!status.ok()) {
                    // TODO: Change to SPDLOG_DEBUG before release
                    SPDLOG_INFO("Model:{} was unavailable during pipeline:{} inputs info fetching", instance->getName(), this->getName());
                    return status;
                }

                for (const auto& [alias, realName] : specificDependencyMapping) {
                    inputsInfo[alias] = instance->getInputsInfo().at(realName);
                }
                break;
            }
            default: {
                // Pipeline validation does not allow connections into entry node.
                SPDLOG_ERROR("Unexpected dependant node kind (name:{})", this->getName());
                return StatusCode::UNKNOWN_ERROR;
            }
            }
        }
    }

    return StatusCode::OK;
}

Status PipelineDefinition::getOutputsInfo(tensor_map_t& outputsInfo, const ModelManager& manager) const {
    // Assumptions: this can only be called on available pipeline definition.
    // Add check if available when pipeline status will be implemented.

    static const auto byName = [](const std::string& name) {
        return [name](const NodeInfo& nodeInfo) {
            return nodeInfo.nodeName == name;
        };
    };

    for (const auto& [dependantNodeName, allMappings] : connections) {
        const auto& dependantNodeInfo = std::find_if(std::begin(nodeInfos), std::end(nodeInfos), byName(dependantNodeName));
        if (dependantNodeInfo->kind != NodeKind::EXIT) {
            continue;
        }

        for (const auto& [dependencyNodeName, specificDependencyMapping] : allMappings) {
            const auto& dependencyNodeInfo = std::find_if(std::begin(nodeInfos), std::end(nodeInfos), byName(dependencyNodeName));

            switch (dependencyNodeInfo->kind) {
            case NodeKind::ENTRY: {
                for (const auto& [alias, realName] : specificDependencyMapping) {
                    outputsInfo.insert({realName, TensorInfo::getUnspecifiedTensorInfo()});
                }
                break;
            }
            case NodeKind::DL: {
                auto instance = manager.findModelInstance(dependencyNodeInfo->modelName, dependencyNodeInfo->modelVersion.value_or(0));
                if (!instance) {
                    // TODO: Change to SPDLOG_DEBUG before release
                    SPDLOG_INFO("Model:{} was unavailable during pipeline:{} outputs info fetching", dependencyNodeInfo->modelName, this->getName());
                    return StatusCode::MODEL_MISSING;
                }
                std::unique_ptr<ModelInstanceUnloadGuard> unloadGuard;
                auto status = instance->waitForLoaded(0, unloadGuard);
                if (!status.ok()) {
                    // TODO: Change to SPDLOG_DEBUG before release
                    SPDLOG_INFO("Model:{} was unavailable during pipeline:{} outputs info fetching", instance->getName(), this->getName());
                    return status;
                }

                for (const auto& [alias, realName] : specificDependencyMapping) {
                    const auto& finalName = dependencyNodeInfo->outputNameAliases.count(alias) > 0 ? dependencyNodeInfo->outputNameAliases.at(alias) : alias;
                    outputsInfo[realName] = instance->getOutputsInfo().at(finalName);
                }
                break;
            }
            default: {
                // Pipeline validation does not allow connections from exit node.
                SPDLOG_ERROR("Unexpected dependency node kind (name:{})", this->getName());
                return StatusCode::UNKNOWN_ERROR;
            }
            }
        }
    }

    return StatusCode::OK;
}

}  // namespace ovms
