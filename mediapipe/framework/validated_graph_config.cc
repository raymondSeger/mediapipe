// Copyright 2019 The MediaPipe Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mediapipe/framework/validated_graph_config.h"

#include <unordered_set>

#include "absl/memory/memory.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/substitute.h"
#include "mediapipe/framework/calculator.pb.h"
#include "mediapipe/framework/calculator_base.h"
#include "mediapipe/framework/calculator_registry_util.h"
#include "mediapipe/framework/legacy_calculator_support.h"
#include "mediapipe/framework/packet_generator.h"
#include "mediapipe/framework/packet_generator.pb.h"
#include "mediapipe/framework/packet_set.h"
#include "mediapipe/framework/packet_type.h"
#include "mediapipe/framework/port.h"
#include "mediapipe/framework/port/core_proto_inc.h"
#include "mediapipe/framework/port/integral_types.h"
#include "mediapipe/framework/port/logging.h"
#include "mediapipe/framework/port/ret_check.h"
#include "mediapipe/framework/port/source_location.h"
#include "mediapipe/framework/port/status.h"
#include "mediapipe/framework/port/status_builder.h"
#include "mediapipe/framework/port/topologicalsorter.h"
#include "mediapipe/framework/status_handler.h"
#include "mediapipe/framework/stream_handler.pb.h"
#include "mediapipe/framework/thread_pool_executor.pb.h"
#include "mediapipe/framework/tool/status_util.h"
#include "mediapipe/framework/tool/subgraph_expansion.h"
#include "mediapipe/framework/tool/validate.h"
#include "mediapipe/framework/tool/validate_name.h"

namespace mediapipe {

namespace {

// Create a debug std::string name for a set of edge.  An edge can be either
// a stream or a side packet.
std::string DebugEdgeNames(
    const std::string& edge_type,
    const proto_ns::RepeatedPtrField<ProtoString>& edges) {
  if (edges.empty()) {
    return absl::StrCat("no ", edge_type, "s");
  }
  if (edges.size() == 1) {
    return absl::StrCat(edge_type, ": ", edges.Get(0));
  }
  return absl::StrCat(edge_type, "s: <", absl::StrJoin(edges, ","), ">");
}

// TODO Shorten the debug name to identify the node with minimal
// information.
std::string DebugName(const CalculatorGraphConfig::Node& node_config) {
  const std::string& name = node_config.name();
  if (name.empty()) {
    return absl::StrCat(
        "[", node_config.calculator(), ", ",
        DebugEdgeNames("input stream", node_config.input_stream()), ", and ",
        DebugEdgeNames("output stream", node_config.output_stream()), "]");
  }
  return name;
}

std::string DebugName(const PacketGeneratorConfig& node_config) {
  return absl::StrCat(
      "[", node_config.packet_generator(), ", ",
      DebugEdgeNames("input side packet", node_config.input_side_packet()),
      ", and ",
      DebugEdgeNames("output side packet", node_config.output_side_packet()),
      "]");
}

std::string DebugName(const StatusHandlerConfig& node_config) {
  return absl::StrCat(
      "[", node_config.status_handler(), ", ",
      DebugEdgeNames("input side packet", node_config.input_side_packet()),
      "]");
}

std::string DebugName(const CalculatorGraphConfig& config,
                      NodeTypeInfo::NodeType node_type, int node_index) {
  switch (node_type) {
    case NodeTypeInfo::NodeType::CALCULATOR:
      return DebugName(config.node(node_index));
    case NodeTypeInfo::NodeType::PACKET_GENERATOR:
      return DebugName(config.packet_generator(node_index));
    case NodeTypeInfo::NodeType::GRAPH_INPUT_STREAM:
      return config.input_stream(node_index);
    case NodeTypeInfo::NodeType::STATUS_HANDLER:
      return DebugName(config.status_handler(node_index));
    case NodeTypeInfo::NodeType::UNKNOWN:
      /* Fall through. */ {}
  }
  LOG(FATAL) << "Unknown NodeTypeInfo::NodeType: "
             << NodeTypeInfo::NodeTypeToString(node_type);
}

// Adds the ExecutorConfigs for predefined executors, if they are not in
// graph_config.
//
// Converts the graph-level num_threads field to an ExecutorConfig for the
// default executor with the executor type unspecified.
::mediapipe::Status AddPredefinedExecutorConfigs(
    CalculatorGraphConfig* graph_config) {
  bool has_default_executor_config = false;
  for (ExecutorConfig& executor_config : *graph_config->mutable_executor()) {
    if (executor_config.name().empty()) {
      if (graph_config->num_threads()) {
        return ::mediapipe::InvalidArgumentError(
            "ExecutorConfig for the default executor and the graph-level "
            "num_threads field should not both be specified.");
      }
      has_default_executor_config = true;
      break;
    }
  }
  if (!has_default_executor_config) {
    ExecutorConfig* default_executor_config = graph_config->add_executor();
    if (graph_config->num_threads()) {
      MediaPipeOptions* options = default_executor_config->mutable_options();
      options->MutableExtension(ThreadPoolExecutorOptions::ext)
          ->set_num_threads(graph_config->num_threads());
      graph_config->clear_num_threads();
    }
  }
  return ::mediapipe::OkStatus();
}

::mediapipe::Status PerformBasicTransforms(
    const CalculatorGraphConfig& input_graph_config,
    const GraphRegistry* graph_registry,
    CalculatorGraphConfig* output_graph_config) {
  *output_graph_config = input_graph_config;
  RETURN_IF_ERROR(tool::ExpandSubgraphs(output_graph_config, graph_registry));

  RETURN_IF_ERROR(AddPredefinedExecutorConfigs(output_graph_config));

  // Populate each node with the graph level input stream handler if a
  // stream handler wasn't explicitly provided.
  // TODO Instead of pre-populating, handle the graph level
  // default appropriately within CalculatorGraph.
  if (output_graph_config->has_input_stream_handler()) {
    const auto& graph_level_input_stream_handler =
        output_graph_config->input_stream_handler();
    for (auto& node : *output_graph_config->mutable_node()) {
      if (!node.has_input_stream_handler()) {
        *node.mutable_input_stream_handler() = graph_level_input_stream_handler;
      }
    }
  }

  return ::mediapipe::OkStatus();
}

}  // namespace

std::string CanonicalNodeName(const CalculatorGraphConfig& graph_config,
                              int node_id) {
  const auto& node_config = graph_config.node(node_id);
  std::string node_name = node_config.name().empty() ? node_config.calculator()
                                                     : node_config.name();
  int count = 0;
  int sequence = 0;
  for (int i = 0; i < graph_config.node_size(); i++) {
    const auto& current_node_config = graph_config.node(i);
    std::string current_node_name = current_node_config.name().empty()
                                        ? current_node_config.calculator()
                                        : current_node_config.name();
    if (node_name == current_node_name) {
      ++count;
      if (i < node_id) {
        ++sequence;
      }
    }
  }
  if (count <= 1) {
    return node_name;
  }
  return absl::StrCat(node_name, "_", sequence + 1);
}

// static
std::string NodeTypeInfo::NodeTypeToString(NodeType node_type) {
  switch (node_type) {
    case NodeTypeInfo::NodeType::CALCULATOR:
      return "Calculator";
    case NodeTypeInfo::NodeType::PACKET_GENERATOR:
      return "Packet Generator";
    case NodeTypeInfo::NodeType::GRAPH_INPUT_STREAM:
      return "Graph Input Stream";
    case NodeTypeInfo::NodeType::STATUS_HANDLER:
      return "Status Handler";
    case NodeTypeInfo::NodeType::UNKNOWN:
      return "Unknown Node";
  }
  LOG(FATAL) << "Unknown NodeTypeInfo::NodeType: "
             << static_cast<int>(node_type);
}

::mediapipe::Status NodeTypeInfo::Initialize(
    const ValidatedGraphConfig& validated_graph,
    const CalculatorGraphConfig::Node& node, int node_index) {
  node_.type = NodeType::CALCULATOR;
  node_.index = node_index;
  RETURN_IF_ERROR(contract_.Initialize(node));
  contract_.SetNodeName(
      CanonicalNodeName(validated_graph.Config(), node_index));

  // Ensure input_stream_info field is well formed.
  if (!node.input_stream_info().empty()) {
    std::vector<bool> id_used(contract_.Inputs().NumEntries(),
                              false);  // Indexed by CollectionItemId.
    for (const auto& input_stream_info : node.input_stream_info()) {
      std::string tag;
      int index;
      RETURN_IF_ERROR(
          tool::ParseTagIndex(input_stream_info.tag_index(), &tag, &index));
      CollectionItemId id = contract_.Inputs().GetId(tag, index);
      if (!id.IsValid()) {
        return ::mediapipe::InvalidArgumentErrorBuilder(MEDIAPIPE_LOC)
               << "Input stream with tag_index \""
               << input_stream_info.tag_index()
               << "\" requested in InputStreamInfo but is not an input stream "
                  "of the calculator.";
      }
      if (id_used[id.value()]) {
        return ::mediapipe::InvalidArgumentErrorBuilder(MEDIAPIPE_LOC)
               << "Input stream with tag_index \""
               << input_stream_info.tag_index()
               << "\" has more than one InputStreamInfo.";
      }
      id_used[id.value()] = true;
    }
  }

  // Run FillExpectations or GetContract.
  const auto& node_class = node.calculator();
  RET_CHECK_EQ(&node.options(), &contract_.Options());
#if !defined(MEDIAPIPE_PROTO_LITE)
  std::set<absl::string_view> type_urls;
  for (const ::mediapipe::protobuf::Any& options : node.node_options()) {
    RET_CHECK(type_urls.insert(options.type_url()).second)
        << "Options type: '" << options.type_url()
        << "' specified more than once for a single calculator node config.";
  }
#endif
  LegacyCalculatorSupport::Scoped<CalculatorContract> s(&contract_);
  RETURN_IF_ERROR(VerifyCalculatorWithContract(validated_graph.Package(),
                                               node_class, &contract_));

  // Validate result of FillExpectations or GetContract.
  std::vector<::mediapipe::Status> statuses;
  ::mediapipe::Status status = ValidatePacketTypeSet(contract_.Inputs());
  if (!status.ok()) {
    statuses.push_back(
        ::mediapipe::StatusBuilder(std::move(status), MEDIAPIPE_LOC)
            .SetPrepend()
        << "For input streams ");
  }
  status = ValidatePacketTypeSet(contract_.Outputs());
  if (!status.ok()) {
    statuses.push_back(
        ::mediapipe::StatusBuilder(std::move(status), MEDIAPIPE_LOC)
            .SetPrepend()
        << "For output streams ");
  }
  status = ValidatePacketTypeSet(contract_.InputSidePackets());
  if (!status.ok()) {
    statuses.push_back(
        ::mediapipe::StatusBuilder(std::move(status), MEDIAPIPE_LOC)
            .SetPrepend()
        << "For input side packets ");
  }
  if (!statuses.empty()) {
    return tool::CombinedStatus(
        absl::StrCat(node_class,
                     IsLegacyCalculator(validated_graph.Package(), node_class)
                         ? "::FillExpectations"
                         : "::GetContract",
                     " failed to validate: "),
        statuses);
  }
  return ::mediapipe::OkStatus();
}

::mediapipe::Status NodeTypeInfo::Initialize(
    const ValidatedGraphConfig& validated_graph,
    const PacketGeneratorConfig& node, int node_index) {
  node_.type = NodeType::PACKET_GENERATOR;
  node_.index = node_index;
  RETURN_IF_ERROR(contract_.Initialize(node));

  // Run FillExpectations.
  const std::string& node_class = node.packet_generator();
  ASSIGN_OR_RETURN(
      auto static_access,
      internal::StaticAccessToGeneratorRegistry::CreateByNameInNamespace(
          validated_graph.Package(), node_class),
      _ << "Unable to find PacketGenerator \"" << node_class << "\"");
  {
    LegacyCalculatorSupport::Scoped<CalculatorContract> s(&contract_);
    RETURN_IF_ERROR(static_access->FillExpectations(
                        node.options(), &contract_.InputSidePackets(),
                        &contract_.OutputSidePackets()))
            .SetPrepend()
        << node_class << ": ";
  }

  // Validate result of FillExpectations.
  std::vector<::mediapipe::Status> statuses;
  ::mediapipe::Status status =
      ValidatePacketTypeSet(contract_.InputSidePackets());
  if (!status.ok()) {
    statuses.push_back(std::move(status));
  }
  status = ValidatePacketTypeSet(contract_.OutputSidePackets());
  if (!status.ok()) {
    statuses.push_back(std::move(status));
  }
  if (!statuses.empty()) {
    return tool::CombinedStatus(
        absl::StrCat(node_class, "::FillExpectations failed to validate: "),
        statuses);
  }
  return ::mediapipe::OkStatus();
}

::mediapipe::Status NodeTypeInfo::Initialize(
    const ValidatedGraphConfig& validated_graph,
    const StatusHandlerConfig& node, int node_index) {
  node_.type = NodeType::STATUS_HANDLER;
  node_.index = node_index;
  RETURN_IF_ERROR(contract_.Initialize(node));

  // Run FillExpectations.
  const std::string& node_class = node.status_handler();
  ASSIGN_OR_RETURN(
      auto static_access,
      internal::StaticAccessToStatusHandlerRegistry::CreateByNameInNamespace(
          validated_graph.Package(), node_class),
      _ << "Unable to find StatusHandler \"" << node_class << "\"");
  {
    LegacyCalculatorSupport::Scoped<CalculatorContract> s(&contract_);
    RETURN_IF_ERROR(static_access->FillExpectations(
                        node.options(), &contract_.InputSidePackets()))
            .SetPrepend()
        << node_class << ": ";
  }

  // Validate result of FillExpectations.
  RETURN_IF_ERROR(ValidatePacketTypeSet(contract_.InputSidePackets()))
          .SetPrepend()
      << node_class << "::FillExpectations failed to validate: ";
  return ::mediapipe::OkStatus();
}

::mediapipe::Status ValidatedGraphConfig::Initialize(
    const CalculatorGraphConfig& input_config,
    const GraphRegistry* graph_registry) {
  RET_CHECK(!initialized_)
      << "ValidatedGraphConfig can be initialized only once.";

#if !defined(MEDIAPIPE_MOBILE)
  VLOG(1) << "ValidatedGraphConfig::Initialize called with config:\n"
          << input_config.DebugString();
#endif

  RETURN_IF_ERROR(
      PerformBasicTransforms(input_config, graph_registry, &config_));

  // Initialize the basic node information.
  RETURN_IF_ERROR(InitializeGeneratorInfo());
  RETURN_IF_ERROR(InitializeCalculatorInfo());
  RETURN_IF_ERROR(InitializeStatusHandlerInfo());

  sorted_nodes_.reserve(generators_.size() + calculators_.size());
  // Initialize sorted_nodes_ to list generators before calculators.
  for (int index = 0; index < generators_.size(); ++index) {
    NodeTypeInfo* node_type_info = &generators_[index];
    RET_CHECK(node_type_info->Node().type ==
              NodeTypeInfo::NodeType::PACKET_GENERATOR);
    RET_CHECK_EQ(node_type_info->Node().index, index);
    sorted_nodes_.push_back(node_type_info);
  }
  for (int index = 0; index < calculators_.size(); ++index) {
    NodeTypeInfo* node_type_info = &calculators_[index];
    RET_CHECK(node_type_info->Node().type ==
              NodeTypeInfo::NodeType::CALCULATOR);
    RET_CHECK_EQ(node_type_info->Node().index, index);
    sorted_nodes_.push_back(node_type_info);
  }

  // Initialize the side packet information.
  bool need_sorting = false;
  RETURN_IF_ERROR(InitializeSidePacketInfo(&need_sorting));
  // Initialize the stream information.
  RETURN_IF_ERROR(InitializeStreamInfo(&need_sorting));
  if (need_sorting) {
    RETURN_IF_ERROR(TopologicalSortNodes());

    // Clear the information from the unsorted analysis.
    side_packet_to_producer_.clear();
    required_side_packets_.clear();
    input_side_packets_.clear();
    output_side_packets_.clear();
    stream_to_producer_.clear();
    input_streams_.clear();
    output_streams_.clear();
    owned_packet_types_.clear();

    // Recompute on sorted graph.
    RETURN_IF_ERROR(InitializeSidePacketInfo(nullptr));
    RETURN_IF_ERROR(InitializeStreamInfo(nullptr));
  }

  // Fill in all the upstream fields now that we are assured of having
  // things in the right order and all the output streams have been
  // created.
  RETURN_IF_ERROR(FillUpstreamFieldForBackEdges());

  // Set Any types based on what they connect to.
  RETURN_IF_ERROR(ResolveAnyTypes(&input_streams_, &output_streams_));
  RETURN_IF_ERROR(ResolveAnyTypes(&input_side_packets_, &output_side_packets_));

  // Validate consistency of side packets and streams.
  RETURN_IF_ERROR(ValidateSidePacketTypes());
  RETURN_IF_ERROR(ValidateStreamTypes());

  RETURN_IF_ERROR(ComputeSourceDependence());

  RETURN_IF_ERROR(ValidateExecutors());

#if !defined(MEDIAPIPE_MOBILE)
  VLOG(1) << "ValidatedGraphConfig produced canonical config:\n"
          << config_.DebugString();
#endif
  initialized_ = true;
  return ::mediapipe::OkStatus();
}

::mediapipe::Status ValidatedGraphConfig::Initialize(
    const std::string& graph_type, const Subgraph::SubgraphOptions* options,
    const GraphRegistry* graph_registry) {
  graph_registry =
      graph_registry ? graph_registry : &GraphRegistry::global_graph_registry;
  auto status_or_config = graph_registry->CreateByName("", graph_type, options);
  RETURN_IF_ERROR(status_or_config.status());
  return Initialize(status_or_config.ValueOrDie(), graph_registry);
}

::mediapipe::Status ValidatedGraphConfig::Initialize(
    const std::vector<CalculatorGraphConfig>& input_configs,
    const std::vector<CalculatorGraphTemplate>& input_templates,
    const std::string& graph_type, const Subgraph::SubgraphOptions* options) {
  GraphRegistry graph_registry;
  for (auto& config : input_configs) {
    graph_registry.Register(config.type(), config);
  }
  for (auto& templ : input_templates) {
    graph_registry.Register(templ.config().type(), templ);
  }
  return Initialize(graph_type, options, &graph_registry);
}

::mediapipe::Status ValidatedGraphConfig::InitializeCalculatorInfo() {
  std::vector<::mediapipe::Status> statuses;
  calculators_.reserve(config_.node_size());
  for (const auto& node : config_.node()) {
    calculators_.emplace_back();
    ::mediapipe::Status status =
        calculators_.back().Initialize(*this, node, calculators_.size() - 1);
    if (!status.ok()) {
      statuses.push_back(status);
    }
  }
  return tool::CombinedStatus("ValidatedGraphConfig Initialization failed.",
                              statuses);
}

::mediapipe::Status ValidatedGraphConfig::InitializeGeneratorInfo() {
  std::vector<::mediapipe::Status> statuses;
  generators_.reserve(config_.packet_generator_size());
  for (const auto& node : config_.packet_generator()) {
    generators_.emplace_back();
    ::mediapipe::Status status =
        generators_.back().Initialize(*this, node, generators_.size() - 1);
    if (!status.ok()) {
      statuses.push_back(status);
    }
  }
  return tool::CombinedStatus("ValidatedGraphConfig Initialization failed.",
                              statuses);
}

::mediapipe::Status ValidatedGraphConfig::InitializeStatusHandlerInfo() {
  std::vector<::mediapipe::Status> statuses;
  status_handlers_.reserve(config_.status_handler_size());
  for (const auto& node : config_.status_handler()) {
    status_handlers_.emplace_back();
    ::mediapipe::Status status = status_handlers_.back().Initialize(
        *this, node, status_handlers_.size() - 1);
    if (!status.ok()) {
      statuses.push_back(status);
    }
  }
  return tool::CombinedStatus("ValidatedGraphConfig Initialization failed.",
                              statuses);
}

::mediapipe::Status ValidatedGraphConfig::InitializeSidePacketInfo(
    bool* need_sorting_ptr) {
  for (NodeTypeInfo* node_type_info : sorted_nodes_) {
    RETURN_IF_ERROR(AddInputSidePacketsForNode(node_type_info));
    RETURN_IF_ERROR(
        AddOutputSidePacketsForNode(node_type_info, need_sorting_ptr));
  }
  if (need_sorting_ptr && *need_sorting_ptr) {
    return ::mediapipe::OkStatus();
  }
  for (int index = 0; index < config_.status_handler_size(); ++index) {
    NodeTypeInfo* node_type_info = &status_handlers_[index];
    RET_CHECK(node_type_info->Node().type ==
              NodeTypeInfo::NodeType::STATUS_HANDLER);
    RET_CHECK_EQ(node_type_info->Node().index, index);
    RETURN_IF_ERROR(AddInputSidePacketsForNode(node_type_info));
  }
  return ::mediapipe::OkStatus();
}

::mediapipe::Status ValidatedGraphConfig::AddInputSidePacketsForNode(
    NodeTypeInfo* node_type_info) {
  node_type_info->SetInputSidePacketBaseIndex(input_side_packets_.size());
  const tool::TagMap& tag_map =
      *node_type_info->InputSidePacketTypes().TagMap();
  for (CollectionItemId id = tag_map.BeginId(); id < tag_map.EndId(); ++id) {
    const std::string& name = tag_map.Names()[id.value()];
    input_side_packets_.emplace_back();
    auto& edge_info = input_side_packets_.back();

    auto iter = side_packet_to_producer_.find(name);
    if (iter != side_packet_to_producer_.end()) {
      // The side packet is generated by something upstream.
      edge_info.upstream = iter->second;
    } else {
      // The side packet must be given to the graph (or the graph isn't
      // topologically sorted).
      required_side_packets_[name].push_back(input_side_packets_.size() - 1);
    }
    edge_info.parent_node = node_type_info->Node();
    edge_info.name = name;
    edge_info.packet_type = &node_type_info->InputSidePacketTypes().Get(id);
  }
  return ::mediapipe::OkStatus();
}

::mediapipe::Status ValidatedGraphConfig::AddOutputSidePacketsForNode(
    NodeTypeInfo* node_type_info, bool* need_sorting_ptr) {
  node_type_info->SetOutputSidePacketBaseIndex(output_side_packets_.size());
  const tool::TagMap& tag_map =
      *node_type_info->OutputSidePacketTypes().TagMap();
  for (CollectionItemId id = tag_map.BeginId(); id < tag_map.EndId(); ++id) {
    const std::string& name = tag_map.Names()[id.value()];
    output_side_packets_.emplace_back();
    auto& edge_info = output_side_packets_.back();

    edge_info.parent_node = node_type_info->Node();
    edge_info.name = name;
    edge_info.packet_type = &node_type_info->OutputSidePacketTypes().Get(id);

    if (!::mediapipe::InsertIfNotPresent(&side_packet_to_producer_, name,
                                         output_side_packets_.size() - 1)) {
      return ::mediapipe::UnknownErrorBuilder(MEDIAPIPE_LOC)
             << "Output Side Packet \"" << name << "\" defined twice.";
    }
    if (::mediapipe::ContainsKey(required_side_packets_, name)) {
      if (need_sorting_ptr) {
        *need_sorting_ptr = true;
        // Don't return early, we still need to gather information about
        // every side packet in order to sort.
      } else {
        return ::mediapipe::UnknownErrorBuilder(MEDIAPIPE_LOC)
               << "Side packet \"" << name
               << "\" was produced after it was used.";
      }
    }
  }
  return ::mediapipe::OkStatus();
}

::mediapipe::Status ValidatedGraphConfig::InitializeStreamInfo(
    bool* need_sorting_ptr) {
  // Define output streams for graph input streams.
  ASSIGN_OR_RETURN(std::shared_ptr<tool::TagMap> graph_input_streams,
                   tool::TagMap::Create(config_.input_stream()));
  for (int index = 0; index < graph_input_streams->Names().size(); ++index) {
    std::string name = graph_input_streams->Names()[index];
    owned_packet_types_.emplace_back(new PacketType());
    owned_packet_types_.back()->SetAny();
    // Indexes for graph input streams are virtual nodes which start
    // after the normal nodes.
    NodeTypeInfo::NodeRef virtual_node{
        NodeTypeInfo::NodeType::GRAPH_INPUT_STREAM,
        index + config_.node_size()};
    RETURN_IF_ERROR(
        AddOutputStream(virtual_node, name, owned_packet_types_.back().get()));
  }

  for (NodeTypeInfo& node_type_info : calculators_) {
    RET_CHECK(node_type_info.Node().type == NodeTypeInfo::NodeType::CALCULATOR);
    // Add input streams before outputs (so back edges from a node to
    // itself must be marked).
    RETURN_IF_ERROR(AddInputStreamsForNode(&node_type_info, need_sorting_ptr));
    RETURN_IF_ERROR(AddOutputStreamsForNode(&node_type_info));
  }

  // Validate tag-name-indexes for graph output streams.
  RETURN_IF_ERROR(tool::TagMap::Create(config_.output_stream()).status());
  return ::mediapipe::OkStatus();
}

::mediapipe::Status ValidatedGraphConfig::AddOutputStreamsForNode(
    NodeTypeInfo* node_type_info) {
  // Define output streams connecting calculators.
  node_type_info->SetOutputStreamBaseIndex(output_streams_.size());
  const tool::TagMap& tag_map = *node_type_info->OutputStreamTypes().TagMap();
  for (CollectionItemId id = tag_map.BeginId(); id < tag_map.EndId(); ++id) {
    RETURN_IF_ERROR(
        AddOutputStream(node_type_info->Node(), tag_map.Names()[id.value()],
                        &node_type_info->OutputStreamTypes().Get(id)));
  }
  return ::mediapipe::OkStatus();
}

::mediapipe::Status ValidatedGraphConfig::AddOutputStream(
    NodeTypeInfo::NodeRef node, const std::string& name,
    PacketType* packet_type) {
  output_streams_.emplace_back();
  auto& edge_info = output_streams_.back();

  edge_info.parent_node = node;
  edge_info.name = name;
  edge_info.packet_type = packet_type;

  if (!::mediapipe::InsertIfNotPresent(&stream_to_producer_, name,
                                       output_streams_.size() - 1)) {
    return ::mediapipe::UnknownErrorBuilder(MEDIAPIPE_LOC)
           << "Output Stream \"" << name << "\" defined twice.";
  }
  return ::mediapipe::OkStatus();
}

::mediapipe::Status ValidatedGraphConfig::AddInputStreamsForNode(
    NodeTypeInfo* node_type_info, bool* need_sorting_ptr) {
  node_type_info->SetInputStreamBaseIndex(input_streams_.size());
  const int node_index = node_type_info->Node().index;
  const PacketTypeSet& input_stream_types = node_type_info->InputStreamTypes();
  std::vector<bool> is_back_edge;  // Indexed by CollectionItemId.
  if (!config_.node(node_index).input_stream_info().empty()) {
    is_back_edge.resize(input_stream_types.NumEntries(), false);
    for (const auto& input_stream_info :
         config_.node(node_index).input_stream_info()) {
      if (input_stream_info.back_edge()) {
        std::string tag;
        int index;
        RETURN_IF_ERROR(
            tool::ParseTagIndex(input_stream_info.tag_index(), &tag, &index));
        CollectionItemId id = input_stream_types.GetId(tag, index);
        RET_CHECK(id.IsValid());
        is_back_edge[id.value()] = true;
      }
    }
  }

  const tool::TagMap& tag_map = *input_stream_types.TagMap();
  for (CollectionItemId id = tag_map.BeginId(); id < tag_map.EndId(); ++id) {
    const std::string& name = tag_map.Names()[id.value()];
    input_streams_.emplace_back();
    auto& edge_info = input_streams_.back();
    edge_info.back_edge = !is_back_edge.empty() && is_back_edge[id.value()];

    auto iter = stream_to_producer_.find(name);
    if (iter != stream_to_producer_.end()) {
      if (edge_info.back_edge) {
        // A back edge was specified, but its output side was already seen.
        if (!need_sorting_ptr) {
          LOG(WARNING) << "Input Stream \"" << name
                       << "\" for node with sorted index " << node_index
                       << " is marked as a back edge, but its output stream is "
                          "already available.  This means it was not necessary "
                          "to mark it as a back edge.";
        }
      } else {
        edge_info.upstream = iter->second;
      }
    } else {
      if (edge_info.back_edge) {
        VLOG(1) << "Encountered expected behavior: the back edge \"" << name
                << "\" for node with (possibly sorted) index " << node_index
                << " has an output stream which we have not yet seen.";
      } else if (need_sorting_ptr) {
        *need_sorting_ptr = true;
        // Continue to process the nodes so we gather enough information
        // for the sort operation.
      } else {
        return ::mediapipe::UnknownErrorBuilder(MEDIAPIPE_LOC)
               << "Input Stream \"" << name << "\" for node with sorted index "
               << node_index << " does not have a corresponding output stream.";
      }
    }

    edge_info.parent_node = node_type_info->Node();
    edge_info.name = name;
    edge_info.packet_type = &node_type_info->InputStreamTypes().Get(id);
  }
  return ::mediapipe::OkStatus();
}

int ValidatedGraphConfig::SorterIndexForNode(NodeTypeInfo::NodeRef node) const {
  switch (node.type) {
    case NodeTypeInfo::NodeType::PACKET_GENERATOR:
      return node.index;
    case NodeTypeInfo::NodeType::CALCULATOR:
      return generators_.size() + node.index;
    default:
      CHECK(false);
  }
}

NodeTypeInfo::NodeRef ValidatedGraphConfig::NodeForSorterIndex(
    int index) const {
  if (index < generators_.size()) {
    return {NodeTypeInfo::NodeType::PACKET_GENERATOR, index};
  } else {
    return {NodeTypeInfo::NodeType::CALCULATOR,
            index - static_cast<int>(generators_.size())};
  }
}

::mediapipe::Status ValidatedGraphConfig::TopologicalSortNodes() {
#if !(defined(MEDIAPIPE_LITE) || defined(MEDIAPIPE_MOBILE))
  VLOG(2) << "BEFORE TOPOLOGICAL SORT:\n" << config_.DebugString();
#endif  // !(MEDIAPIPE_LITE || MEDIAPIPE_MOBILE)
  // The topological sorter assumes the nodes in the graph are identified
  // by consecutive indexes 0, 1, 2, ... We sort the generators and
  // calculators. Their indexes for the topological sorter are assigned as
  // follows:
  // - We use the generator indexes directly.
  // - We shift the calculator indexes up by the number of generators.
  TopologicalSorter sorter(generators_.size() + calculators_.size());
  for (int index = 0; index < input_streams_.size(); ++index) {
    const std::string& name = input_streams_[index].name;
    // The upstream field may be broken since the order was wrong, so
    // look it up directly (now that we've filled stream_to_producer_).
    auto iter = stream_to_producer_.find(name);
    if (iter != stream_to_producer_.end()) {
      int upstream = iter->second;
      // Ignore graph input streams and back edges.
      if (output_streams_[upstream].parent_node.type !=
              NodeTypeInfo::NodeType::GRAPH_INPUT_STREAM &&
          !input_streams_[index].back_edge) {
        VLOG(3) << "Adding an edge for stream \"" << name << "\" from "
                << output_streams_[upstream].parent_node.index << " to "
                << input_streams_[index].parent_node.index;
        sorter.AddEdge(
            SorterIndexForNode(output_streams_[upstream].parent_node),
            SorterIndexForNode(input_streams_[index].parent_node));
      }
    }
  }
  for (int index = 0; index < input_side_packets_.size(); ++index) {
    if (input_side_packets_[index].parent_node.type !=
            NodeTypeInfo::NodeType::PACKET_GENERATOR &&
        input_side_packets_[index].parent_node.type !=
            NodeTypeInfo::NodeType::CALCULATOR) {
      continue;
    }
    const std::string& name = input_side_packets_[index].name;
    // The upstream field may be broken since the order was wrong, so
    // look it up directly (now that we've filled side_packet_to_producer_).
    auto iter = side_packet_to_producer_.find(name);
    if (iter != side_packet_to_producer_.end()) {
      int upstream = iter->second;
      VLOG(3) << "Adding an edge for side packet \"" << name << "\" from "
              << output_side_packets_[upstream].parent_node.index << " to "
              << input_side_packets_[index].parent_node.index;
      sorter.AddEdge(
          SorterIndexForNode(output_side_packets_[upstream].parent_node),
          SorterIndexForNode(input_side_packets_[index].parent_node));
    }
  }

  proto_ns::RepeatedPtrField<PacketGeneratorConfig> generator_configs;
  std::vector<NodeTypeInfo> tmp_generators;
  tmp_generators.reserve(generators_.size());
  generator_configs.Reserve(generators_.size());

  proto_ns::RepeatedPtrField<CalculatorGraphConfig::Node> node_configs;
  std::vector<NodeTypeInfo> tmp_calculators;
  tmp_calculators.reserve(calculators_.size());
  node_configs.Reserve(calculators_.size());

  sorted_nodes_.clear();
  int index;
  bool cyclic = false;
  std::vector<int> cycle_indexes;
  while (sorter.GetNext(&index, &cyclic, &cycle_indexes)) {
    NodeTypeInfo::NodeRef node = NodeForSorterIndex(index);
    if (node.type == NodeTypeInfo::NodeType::PACKET_GENERATOR) {
      VLOG(3) << "Taking generator with index " << node.index
              << " in the original order";
      tmp_generators.emplace_back(std::move(generators_[node.index]));
      tmp_generators.back().SetNodeIndex(tmp_generators.size() - 1);
      generator_configs.Add()->Swap(
          config_.mutable_packet_generator(node.index));
      sorted_nodes_.push_back(&tmp_generators.back());
    } else {
      VLOG(3) << "Taking calculator with index " << node.index
              << " in the original order";
      tmp_calculators.emplace_back(std::move(calculators_[node.index]));
      tmp_calculators.back().SetNodeIndex(tmp_calculators.size() - 1);
      node_configs.Add()->Swap(config_.mutable_node(node.index));
      sorted_nodes_.push_back(&tmp_calculators.back());
    }
  }
  generator_configs.Swap(config_.mutable_packet_generator());
  tmp_generators.swap(generators_);
  node_configs.Swap(config_.mutable_node());
  tmp_calculators.swap(calculators_);
  if (cyclic) {
    return ::mediapipe::UnknownErrorBuilder(MEDIAPIPE_LOC)
           << "Generator side packet cycle or calculator stream cycle detected "
              "in graph.  Cycle indexes: "
           << absl::StrJoin(cycle_indexes, ", ");
  }
#if !(defined(MEDIAPIPE_LITE) || defined(MEDIAPIPE_MOBILE))
  VLOG(2) << "AFTER TOPOLOGICAL SORT:\n" << config_.DebugString();
#endif  // !(MEDIAPIPE_LITE || MEDIAPIPE_MOBILE)
  return ::mediapipe::OkStatus();
}

::mediapipe::Status ValidatedGraphConfig::FillUpstreamFieldForBackEdges() {
  for (int index = 0; index < input_streams_.size(); ++index) {
    auto& input_stream = input_streams_[index];
    if (input_stream.back_edge) {
      RET_CHECK_EQ(-1, input_stream.upstream)
          << "Shouldn't have been able to know the upstream index for back edge"
          << input_stream.name << ".";
      auto iter = stream_to_producer_.find(input_stream.name);
      RET_CHECK(iter != stream_to_producer_.end())
          << "Unable to find upstream edge for back edge \""
          << input_stream.name << "\" (shouldn't have passed validation).";
      // Set the upstream edge.
      input_stream.upstream = iter->second;
    }
  }
  return ::mediapipe::OkStatus();
}

::mediapipe::Status ValidatedGraphConfig::ValidateSidePacketTypes() {
  for (const auto& side_packet : input_side_packets_) {
    // TODO Add a check to ensure multiple input side packets
    // connected to a side packet that will be provided later all have
    // consistent type.
    if (side_packet.upstream != -1 &&
        !side_packet.packet_type->IsConsistentWith(
            *output_side_packets_[side_packet.upstream].packet_type)) {
      return ::mediapipe::UnknownError(absl::Substitute(
          "Input side packet \"$0\" of $1 \"$2\" expected a packet of type "
          "\"$3\" but the connected output side packet will be of type \"$4\"",
          side_packet.name,
          NodeTypeInfo::NodeTypeToString(side_packet.parent_node.type),
          mediapipe::DebugName(config_, side_packet.parent_node.type,
                               side_packet.parent_node.index),
          side_packet.packet_type->DebugTypeName(),
          output_side_packets_[side_packet.upstream]
              .packet_type->DebugTypeName()));
    }
  }
  return ::mediapipe::OkStatus();
}

::mediapipe::Status ValidatedGraphConfig::ResolveAnyTypes(
    std::vector<EdgeInfo>* input_edges, std::vector<EdgeInfo>* output_edges) {
  for (EdgeInfo& input_edge : *input_edges) {
    if (input_edge.upstream == -1) {
      continue;
    }
    EdgeInfo& output_edge = (*output_edges)[input_edge.upstream];
    PacketType* input_root = input_edge.packet_type->GetSameAs();
    PacketType* output_root = output_edge.packet_type->GetSameAs();
    if (input_root->IsAny()) {
      input_root->SetSameAs(output_edge.packet_type);
    } else if (output_root->IsAny()) {
      output_root->SetSameAs(input_edge.packet_type);
    }
  }
  return ::mediapipe::OkStatus();
}

::mediapipe::Status ValidatedGraphConfig::ValidateStreamTypes() {
  for (const EdgeInfo& stream : input_streams_) {
    RET_CHECK_NE(stream.upstream, -1);
    if (!stream.packet_type->IsConsistentWith(
            *output_streams_[stream.upstream].packet_type)) {
      return ::mediapipe::UnknownError(absl::Substitute(
          "Input stream \"$0\" of calculator \"$1\" expects packets of type "
          "\"$2\" but the connected output stream will contain packets of type "
          "\"$3\"",
          stream.name,
          mediapipe::DebugName(config_.node(stream.parent_node.index)),
          stream.packet_type->DebugTypeName(),
          output_streams_[stream.upstream].packet_type->DebugTypeName()));
    }
  }
  return ::mediapipe::OkStatus();
}

::mediapipe::Status ValidatedGraphConfig::ValidateExecutors() {
  std::unordered_set<ProtoString> declared_names;
  for (const ExecutorConfig& executor_config : config_.executor()) {
    if (IsReservedExecutorName(executor_config.name())) {
      return ::mediapipe::InvalidArgumentErrorBuilder(MEDIAPIPE_LOC)
             << "\"" << executor_config.name()
             << "\" is a reserved executor name.";
    }
    if (!declared_names.emplace(executor_config.name()).second) {
      if (executor_config.name().empty()) {
        return ::mediapipe::InvalidArgumentError(
            "ExecutorConfig for the default executor is duplicate.");
      } else {
        return ::mediapipe::InvalidArgumentErrorBuilder(MEDIAPIPE_LOC)
               << "ExecutorConfig for \"" << executor_config.name()
               << "\" is duplicate.";
      }
    }
  }
  for (const CalculatorGraphConfig::Node& node_config : config_.node()) {
    if (node_config.executor().empty()) {
      continue;
    }
    const ProtoString& executor_name = node_config.executor();
    if (IsReservedExecutorName(executor_name)) {
      // TODO: We may want to allow this. For example, we may want to run
      // a non-GPU calculator on the GPU thread for efficiency reasons.
      return ::mediapipe::InvalidArgumentErrorBuilder(MEDIAPIPE_LOC)
             << "\"" << executor_name << "\" is a reserved executor name.";
    }
    // The executor must be declared in an ExecutorConfig.
    if (declared_names.find(executor_name) == declared_names.end()) {
      return ::mediapipe::InvalidArgumentErrorBuilder(MEDIAPIPE_LOC)
             << "The executor \"" << executor_name
             << "\" is not declared in an ExecutorConfig.";
    }
  }
  return ::mediapipe::OkStatus();
}

// static
bool ValidatedGraphConfig::IsReservedExecutorName(const std::string& name) {
  return name == "default" || name == "gpu" || absl::StartsWith(name, "__");
}

::mediapipe::Status ValidatedGraphConfig::ValidateRequiredSidePackets(
    const std::map<std::string, Packet>& side_packets) const {
  std::vector<::mediapipe::Status> statuses;
  for (const auto& required_item : required_side_packets_) {
    auto iter = side_packets.find(required_item.first);
    if (iter == side_packets.end()) {
      statuses.push_back(::mediapipe::InvalidArgumentErrorBuilder(MEDIAPIPE_LOC)
                         << "Side packet \"" << required_item.first
                         << "\" is required but was not provided.");
      continue;
    }
    for (int index : required_item.second) {
      ::mediapipe::Status status =
          input_side_packets_[index].packet_type->Validate(iter->second);
      if (!status.ok()) {
        statuses.push_back(
            ::mediapipe::StatusBuilder(std::move(status), MEDIAPIPE_LOC)
                .SetPrepend()
            << "Side packet \"" << required_item.first
            << "\" failed validation: ");
      }
    }
  }
  if (!statuses.empty()) {
    return tool::CombinedStatus(
        "ValidateRequiredSidePackets failed to validate: ", statuses);
  }
  return ::mediapipe::OkStatus();
}

::mediapipe::Status ValidatedGraphConfig::ValidateRequiredSidePacketTypes(
    const std::map<std::string, PacketType>& side_packet_types) const {
  std::vector<::mediapipe::Status> statuses;
  for (const auto& required_item : required_side_packets_) {
    auto iter = side_packet_types.find(required_item.first);
    if (iter == side_packet_types.end()) {
      statuses.push_back(::mediapipe::InvalidArgumentErrorBuilder(MEDIAPIPE_LOC)
                         << "Side packet \"" << required_item.first
                         << "\" is required but was not provided.");
      continue;
    }
    for (int index : required_item.second) {
      if (!input_side_packets_[index].packet_type->IsConsistentWith(
              iter->second)) {
        return ::mediapipe::UnknownErrorBuilder(MEDIAPIPE_LOC)
               << "Side packet \"" << required_item.first
               << "\" has incorrect type.";
      }
    }
  }
  if (!statuses.empty()) {
    return tool::CombinedStatus(
        "ValidateRequiredSidePackets failed to validate: ", statuses);
  }
  return ::mediapipe::OkStatus();
}

::mediapipe::Status ValidatedGraphConfig::ComputeSourceDependence() {
  for (int node_index = 0; node_index < calculators_.size(); ++node_index) {
    NodeTypeInfo& node_type_info = calculators_[node_index];
    if (node_type_info.InputStreamTypes().NumEntries() == 0) {
      node_type_info.AddSource(node_index);
    } else {
      // For each input stream (index in the flat array).
      for (int stream_index = node_type_info.InputStreamBaseIndex();
           stream_index < node_type_info.InputStreamBaseIndex() +
                              node_type_info.InputStreamTypes().NumEntries();
           ++stream_index) {
        // Get all the sources of the upstream node.
        RET_CHECK(stream_index >= 0 && stream_index < input_streams_.size())
            << "Unable to find input streams for non-source node with index "
            << node_index << " tried to use " << stream_index;
        const EdgeInfo& input_edge_info = input_streams_[stream_index];
        RET_CHECK_LE(0, input_edge_info.upstream)
            << "input stream \"" << input_edge_info.name
            << "\" is not connected to an output stream.";
        const EdgeInfo& output_edge_info =
            output_streams_[input_edge_info.upstream];
        RET_CHECK_LE(0, output_edge_info.parent_node.index)
            << "output stream \"" << output_edge_info.name
            << "\" does not have a valid node which owns it.";
        RET_CHECK_LE(output_edge_info.parent_node.index,
                     calculators_.size() + config_.input_stream_size())
            << "output stream \"" << output_edge_info.name
            << "\" does not have a valid node which owns it.";
        if (output_edge_info.parent_node.type ==
            NodeTypeInfo::NodeType::GRAPH_INPUT_STREAM) {
          // Add the virtual node for the graph input stream.
          node_type_info.AddSource(output_edge_info.parent_node.index);
          continue;
        }
        for (int source : calculators_[output_edge_info.parent_node.index]
                              .AncestorSources()) {
          node_type_info.AddSource(source);
        }
      }
    }
  }
  return ::mediapipe::OkStatus();
}

::mediapipe::StatusOr<std::string>
ValidatedGraphConfig::RegisteredSidePacketTypeName(const std::string& name) {
  auto iter = side_packet_to_producer_.find(name);
  bool defined = false;
  if (iter != side_packet_to_producer_.end()) {
    defined = true;
    const EdgeInfo& output_edge = output_side_packets_[iter->second];
    if (output_edge.packet_type) {
      const std::string* registered_type =
          output_edge.packet_type->RegisteredTypeName();
      if (registered_type) {
        return *registered_type;
      }
    }
  }

  for (const EdgeInfo& input_edge : input_side_packets_) {
    if (input_edge.name == name) {
      defined = true;
      if (input_edge.packet_type) {
        const std::string* registered_type =
            input_edge.packet_type->RegisteredTypeName();
        if (registered_type) {
          return *registered_type;
        }
      }
    }
  }

  if (!defined) {
    return ::mediapipe::InvalidArgumentErrorBuilder(MEDIAPIPE_LOC)
           << "Side packet \"" << name << "\" is not defined in the config.";
  }
  return ::mediapipe::UnknownErrorBuilder(MEDIAPIPE_LOC)
         << "Unable to find the type for side packet \"" << name
         << "\".  It may be set to AnyType or something else that isn't "
            "determinable, or the type may be defined but not registered.";
}

::mediapipe::StatusOr<std::string>
ValidatedGraphConfig::RegisteredStreamTypeName(const std::string& name) {
  auto iter = stream_to_producer_.find(name);
  if (iter == stream_to_producer_.end()) {
    return ::mediapipe::InvalidArgumentErrorBuilder(MEDIAPIPE_LOC)
           << "Stream \"" << name << "\" is not defined in the config.";
  }
  int output_edge_index = iter->second;
  const EdgeInfo& output_edge = output_streams_[output_edge_index];
  if (output_edge.packet_type) {
    const std::string* registered_type =
        output_edge.packet_type->RegisteredTypeName();
    if (registered_type) {
      return *registered_type;
    }
  }

  for (const EdgeInfo& input_edge : input_streams_) {
    if (input_edge.upstream == output_edge_index) {
      if (input_edge.packet_type) {
        const std::string* registered_type =
            input_edge.packet_type->RegisteredTypeName();
        if (registered_type) {
          return *registered_type;
        }
      }
    }
  }
  return ::mediapipe::UnknownErrorBuilder(MEDIAPIPE_LOC)
         << "Unable to find the type for stream \"" << name
         << "\".  It may be set to AnyType or something else that isn't "
            "determinable, or the type may be defined but not registered.";
}

}  // namespace mediapipe
