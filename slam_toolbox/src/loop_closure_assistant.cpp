/*
 * loop_closure_assistant
 * Copyright (c) 2019, Samsung Research America
 *
 * THE WORK (AS DEFINED BELOW) IS PROVIDED UNDER THE TERMS OF THIS CREATIVE
 * COMMONS PUBLIC LICENSE ("CCPL" OR "LICENSE"). THE WORK IS PROTECTED BY
 * COPYRIGHT AND/OR OTHER APPLICABLE LAW. ANY USE OF THE WORK OTHER THAN AS
 * AUTHORIZED UNDER THIS LICENSE OR COPYRIGHT LAW IS PROHIBITED.
 *
 * BY EXERCISING ANY RIGHTS TO THE WORK PROVIDED HERE, YOU ACCEPT AND AGREE TO
 * BE BOUND BY THE TERMS OF THIS LICENSE. THE LICENSOR GRANTS YOU THE RIGHTS
 * CONTAINED HERE IN CONSIDERATION OF YOUR ACCEPTANCE OF SUCH TERMS AND
 * CONDITIONS.
 *
 */

/* Author: Steven Macenski */

#include "slam_toolbox/loop_closure_assistant.hpp"

namespace loop_closure_assistant
{

/*****************************************************************************/
LoopClosureAssistant::LoopClosureAssistant(
  ros::NodeHandle& node,
  karto::Mapper* mapper,
  laser_utils::ScanHolder* scan_holder,
  PausedState& state, ProcessType & processor_type)
: mapper_(mapper), scan_holder_(scan_holder),
  interactive_mode_(false), nh_(node), state_(state),
  processor_type_(processor_type)
/*****************************************************************************/
{
  node.setParam("paused_processing", false);
  tfB_ = std::make_unique<tf2_ros::TransformBroadcaster>();
  ssClear_manual_ = node.advertiseService("clear_changes",
    &LoopClosureAssistant::clearChangesCallback, this);
  ssLoopClosure_ = node.advertiseService("manual_loop_closure",
    &LoopClosureAssistant::manualLoopClosureCallback, this);
  scan_publisher_ = node.advertise<sensor_msgs::LaserScan>(
    "karto_scan_visualization",10);
  solver_ = mapper_->getScanSolver();
  interactive_server_ =
    std::make_unique<interactive_markers::InteractiveMarkerServer>(
    "slam_toolbox","",true);
  ssInteractive_ = node.advertiseService("toggle_interactive_mode",
    &LoopClosureAssistant::interactiveModeCallback,this);
  node.setParam("interactive_mode", interactive_mode_);
  marker_publisher_ = node.advertise<visualization_msgs::MarkerArray>(
    "karto_graph_visualization",1);
  node.param("map_frame", map_frame_, std::string("map"));
  node.param("enable_interactive_mode", enable_interactive_mode_, false);
}

/*****************************************************************************/
void LoopClosureAssistant::setMapper(karto::Mapper * mapper)
/*****************************************************************************/
{
  mapper_ = mapper;
}

/*****************************************************************************/
void LoopClosureAssistant::processInteractiveFeedback(const
  visualization_msgs::InteractiveMarkerFeedbackConstPtr& feedback)
/*****************************************************************************/
{
  // if (processor_type_ != PROCESS)
  // {
  //   ROS_ERROR_THROTTLE(5.,
  //     "Interactive mode is invalid outside processing mode.");
  //   return;
  // }

  const int id = std::stoi(feedback->marker_name, nullptr, 10) - 1;

  // was depressed, something moved, and now released
  if (feedback->event_type ==
      visualization_msgs::InteractiveMarkerFeedback::MOUSE_UP && 
      feedback->mouse_point_valid)
  {
    // addMovedNodes(id, Eigen::Vector3d(feedback->mouse_point.x,
    //   feedback->mouse_point.y, tf2::getYaw(feedback->pose.orientation)));
  }

  int static last_id = -1;
  int static click_count = 0;
  if (feedback->event_type ==
      visualization_msgs::InteractiveMarkerFeedback::MOUSE_DOWN && 
      feedback->mouse_point_valid)
  {
    if (last_id == id)
    {
      click_count++;
    }
    else
    {
      click_count = 1;
    }
    last_id = id;

    ROS_WARN_STREAM("Click count " << click_count << ", node: " << id);
  }

  bool delete_lc = false;

  // was depressed, something moved, and now released
  if (feedback->event_type ==
      visualization_msgs::InteractiveMarkerFeedback::MOUSE_DOWN && 
      feedback->mouse_point_valid)
  {
    if (click_count < 3)
    {
      return;
    }

    karto::Edge<karto::LocalizedRangeScan>* lc_edge = NULL;
    karto::Vertex<karto::LocalizedRangeScan>* vert = mapper_->GetGraph()->GetVertices().begin()->second.at(id);
    if (vert->GetAdjacentVertices().size() > 2)
    {
      for (const auto& edge : vert->GetEdges())
      {
        if (edge->GetSource()->GetAdjacentVertices().size() > 2
            && edge->GetTarget()->GetAdjacentVertices().size() > 2)
        {
          ROS_WARN_STREAM("Found Loop Closure Edge");
          lc_edge = edge;
        }
      }
    }

    if (lc_edge != NULL)
    {
      delete_lc = true;
      click_count = 0;

      {
        auto it = std::find(lc_edge->GetSource()->GetEdges().begin(), lc_edge->GetSource()->GetEdges().end(), lc_edge);
        if (it != lc_edge->GetSource()->GetEdges().end())
        {
          ROS_WARN_STREAM("Deleting Edge from Source");
          int index = std::distance(lc_edge->GetSource()->GetEdges().begin(), it);
          lc_edge->GetSource()->RemoveEdge(index);
        }
      }
      {
        auto it = std::find(lc_edge->GetTarget()->GetEdges().begin(), lc_edge->GetTarget()->GetEdges().end(), lc_edge);
        if (it != lc_edge->GetTarget()->GetEdges().end())
        {
          ROS_WARN_STREAM("Deleting Edge from Target");
          int index = std::distance(lc_edge->GetTarget()->GetEdges().begin(), it);
          lc_edge->GetTarget()->RemoveEdge(index);
        }
      }

      for (int i = 0; i < mapper_->GetGraph()->GetEdges().size(); i++)
      {
        if (lc_edge == mapper_->GetGraph()->GetEdges().at(i))
        {
          ROS_WARN_STREAM("Removing Edge from Graph");
          mapper_->GetGraph()->RemoveEdge(i);
          break;
        }
      }

      mapper_->getScanSolver()->RemoveConstraint(lc_edge->GetSource()->GetObject()->GetStateId(),
        lc_edge->GetTarget()->GetObject()->GetStateId());
    }
  }

  if (feedback->event_type ==
      visualization_msgs::InteractiveMarkerFeedback::MOUSE_DOWN && 
      feedback->mouse_point_valid)
  {
    if (click_count < 3 || delete_lc)
    {
      return;
    }
    click_count = 0;

    karto::Vertex<karto::LocalizedRangeScan>* vert = mapper_->GetGraph()->GetVertices().begin()->second.at(id);
    bool found_closure = mapper_->TryCloseLoop(vert->GetObject(), vert->GetObject()->GetSensorName());

    ROS_WARN_STREAM("Found Closure " << (found_closure ? "true" : "false"));
  }

  // is currently depressed, being moved before release
  if (feedback->event_type ==
      visualization_msgs::InteractiveMarkerFeedback::POSE_UPDATE)
  {
    // get scan
    sensor_msgs::LaserScan scan = scan_holder_->getCorrectedScan(id);

    // get correct orientation
    tf2::Quaternion quat(0.,0.,0.,1.0), msg_quat(0.,0.,0.,1.0);
    double node_yaw, first_node_yaw;
    solver_->GetNodeOrientation(id, node_yaw);
    solver_->GetNodeOrientation(0, first_node_yaw);
    tf2::Quaternion q1(0.,0.,0.,1.0);
    q1.setEuler(0., 0., node_yaw - 3.14159);
    tf2::Quaternion q2(0.,0.,0.,1.0);
    q2.setEuler(0., 0., 3.14159); 
    quat *= q1;
    quat *= q2;

    // interactive move
    tf2::convert(feedback->pose.orientation, msg_quat);
    quat *= msg_quat;
    quat.normalize();

    // create correct transform
    tf2::Transform transform;
    transform.setOrigin(tf2::Vector3(feedback->pose.position.x,
      feedback->pose.position.y, 0.));
    transform.setRotation(quat);

    static int nsec_offset = 1000;
    nsec_offset--;

    // publish the scan visualization with transform
    geometry_msgs::TransformStamped msg;
    tf2::convert(transform, msg.transform);
    msg.child_frame_id = "karto_scan_visualization";
    msg.header.frame_id = feedback->header.frame_id;
    msg.header.stamp = ros::Time::now() + ros::Duration(0, nsec_offset);
    tfB_->sendTransform(msg);

    scan.header.frame_id = "karto_scan_visualization";
    scan.header.stamp = ros::Time::now() + ros::Duration(0, nsec_offset);
    scan_publisher_.publish(scan);
  }
}

/*****************************************************************************/
void LoopClosureAssistant::publishGraph()
/*****************************************************************************/
{
  interactive_server_->clear();
  karto::MapperGraph * graph = mapper_->GetGraph();

  if (!graph || graph->GetVertices().empty())
  {
    return;
  }

  using ConstVertexMapIterator =
      karto::MapperGraph::VertexMap::const_iterator;
  const karto::MapperGraph::VertexMap& vertices = graph->GetVertices();

  int graph_size = 0;
  for (ConstVertexMapIterator it = vertices.begin(); it != vertices.end(); ++it)
  {
    graph_size += it->second.size();
  }
  ROS_DEBUG("Graph size: %i", graph_size);

  bool interactive_mode = false;
  {
    boost::mutex::scoped_lock lock(interactive_mutex_);
    interactive_mode = interactive_mode_;
  }

  const size_t current_marker_count = marker_array_.markers.size();
  marker_array_.markers.clear();  // restart the marker count

  visualization_msgs::Marker vertex_marker =
    vis_utils::toVertexMarker(map_frame_, "slam_toolbox", 0.1);

  for (ConstVertexMapIterator outer_it = vertices.begin();
       outer_it != vertices.end(); ++outer_it)
  {
    using ConstVertexMapValueIterator =
        karto::MapperGraph::VertexMap::mapped_type::const_iterator;
    for (ConstVertexMapValueIterator inner_it = outer_it->second.begin();
         inner_it != outer_it->second.end(); ++inner_it)
    {
      karto::LocalizedRangeScan * scan = inner_it->second->GetObject();

      vertex_marker.pose.position.x = scan->GetCorrectedPose().GetX();
      vertex_marker.pose.position.y = scan->GetCorrectedPose().GetY();

      if (interactive_mode && enable_interactive_mode_)
      {
        // need a 1-to-1 mapping between marker IDs and
        // scan unique IDs to process interactive feedback
        vertex_marker.scale.x = .15;
        vertex_marker.scale.y = .15;
        vertex_marker.scale.z = .15;
        vertex_marker.id = scan->GetUniqueId() + 1;
        visualization_msgs::InteractiveMarker int_marker =
          vis_utils::toInteractiveMarker(vertex_marker, 0.3);
        interactive_server_->insert(int_marker,
          boost::bind(
          &LoopClosureAssistant::processInteractiveFeedback,
          this, _1));
      }
      else
      {
        // use monotonically increasing vertex marker IDs to
        // make room for edge marker IDs
        vertex_marker.id = marker_array_.markers.size();
        marker_array_.markers.push_back(vertex_marker);
      }
    }
  }

  if (!interactive_mode)
  {
    using EdgeList = std::vector<karto::Edge<karto::LocalizedRangeScan>*>;
    using ConstEdgeListIterator = EdgeList::const_iterator;

    visualization_msgs::Marker edge_marker =
      vis_utils::toEdgeMarker(map_frame_, "slam_toolbox", 0.01);

    const EdgeList& edges = graph->GetEdges();
    for (ConstEdgeListIterator it = edges.begin(); it != edges.end(); ++it)
    {
      const karto::Edge<karto::LocalizedRangeScan> * edge = *it;
      karto::LocalizedRangeScan * source_scan = edge->GetSource()->GetObject();
      karto::LocalizedRangeScan * target_scan = edge->GetTarget()->GetObject();
      const karto::Pose2 source_pose = source_scan->GetCorrectedPose();
      const karto::Pose2 target_pose = target_scan->GetCorrectedPose();

      edge_marker.id = marker_array_.markers.size();
      edge_marker.points[0].x = source_pose.GetX();
      edge_marker.points[0].y = source_pose.GetY();
      edge_marker.points[1].x = target_pose.GetX();
      edge_marker.points[1].y = target_pose.GetY();

      bool consecutive_scans = target_scan->GetStateId() - source_scan->GetStateId() == 1;
      double distance = hypot(edge_marker.points[0].x - edge_marker.points[1].x, edge_marker.points[0].y - edge_marker.points[1].y);

      // Crude check to find edges from continue mapping
      if (consecutive_scans && distance > 1.5)
      {
        // Those edges can cause faulty constrains so we delete them
        deleteEdge(edge);
        edge_marker.color.a = 1;
        edge_marker.color.r = 1;
        edge_marker.color.g = 0;
        edge_marker.color.b = 0;
      }
      else if (consecutive_scans)
      {
        edge_marker.color.a = 1;
        edge_marker.color.r = 0;
        edge_marker.color.g = 0;
        edge_marker.color.b = 1;
      }
      else
      {
        edge_marker.color.a = 1;
        edge_marker.color.r = 0;
        edge_marker.color.g = 1;
        edge_marker.color.b = 0;
      }

      marker_array_.markers.push_back(edge_marker);
    }
  }

  const size_t next_marker_count = marker_array_.markers.size();

  // append preexisting markers to force deletion
  while (marker_array_.markers.size() < current_marker_count)
  {
    visualization_msgs::Marker deleted_marker;
    deleted_marker.id = marker_array_.markers.size();
    deleted_marker.action = visualization_msgs::Marker::DELETE;
    marker_array_.markers.push_back(deleted_marker);
  }

  // if disabled, clears out old markers
  interactive_server_->applyChanges();
  marker_publisher_.publish(marker_array_);

  // drop trailing deleted markers
  marker_array_.markers.resize(next_marker_count);
  return;
}

/*****************************************************************************/
bool LoopClosureAssistant::manualLoopClosureCallback(
  slam_toolbox_msgs::LoopClosure::Request& req,
  slam_toolbox_msgs::LoopClosure::Response& resp)
/*****************************************************************************/
{
  if(!enable_interactive_mode_)
  {
    ROS_WARN("Called manual loop closure"
      " with interactive mode disabled. Ignoring.");
    return false;
  }

  {
    boost::mutex::scoped_lock lock(moved_nodes_mutex_);

    // if (moved_nodes_.size() == 0)
    // {
    //   ROS_WARN("No moved nodes to attempt manual loop closure.");
    //   return true;
    // }

    // ROS_INFO("LoopClosureAssistant: Attempting to manual "
    //   "loop close with %i moved nodes.", (int)moved_nodes_.size());
    // // for each in node map
    // std::map<int, Eigen::Vector3d>::const_iterator it = moved_nodes_.begin();
    // for (it; it != moved_nodes_.end(); ++it)
    // {
    //   moveNode(it->first,
    //     Eigen::Vector3d(it->second(0),it->second(1), it->second(2)));
    // }
  }

  // optimize
  mapper_->CorrectPoses();

  // update visualization and clear out nodes completed
  publishGraph();
  clearMovedNodes();
  return true;
}

/*****************************************************************************/
bool LoopClosureAssistant::interactiveModeCallback(
  slam_toolbox_msgs::ToggleInteractive::Request  &req,
  slam_toolbox_msgs::ToggleInteractive::Response &resp)
/*****************************************************************************/
{
  if(!enable_interactive_mode_)
  {
    ROS_WARN("Called toggle interactive mode with "
      "interactive mode disabled. Ignoring.");
    return false;
  }

  bool interactive_mode;
  {
    boost::mutex::scoped_lock lock_i(interactive_mutex_);
    interactive_mode_ = !interactive_mode_;   
    interactive_mode = interactive_mode_;
    nh_.setParam("interactive_mode", interactive_mode_);
  }

  ROS_INFO("SlamToolbox: Toggling %s interactive mode.", 
    interactive_mode ? "on" : "off");
  publishGraph();
  clearMovedNodes();

  // set state so we don't overwrite changes in rviz while loop closing
  state_.set(PROCESSING, interactive_mode);
  state_.set(VISUALIZING_GRAPH, interactive_mode);
  nh_.setParam("paused_processing", interactive_mode);
  return true;
}

/*****************************************************************************/
void LoopClosureAssistant::moveNode(
  const int& id, const Eigen::Vector3d& pose)
/*****************************************************************************/
{
  solver_->ModifyNode(id, pose);
}

/*****************************************************************************/
bool LoopClosureAssistant::clearChangesCallback(
  slam_toolbox_msgs::Clear::Request& req,
  slam_toolbox_msgs::Clear::Response& resp)
/*****************************************************************************/
{
  if(!enable_interactive_mode_)
  {
    ROS_WARN("Called Clear changes with interactive mode disabled. Ignoring.");
    return false;
  }

  ROS_INFO("LoopClosureAssistant: Clearing manual loop closure nodes.");
  publishGraph();
  clearMovedNodes();
  return true;
}

/*****************************************************************************/
void  LoopClosureAssistant::clearMovedNodes()
/*****************************************************************************/
{
  boost::mutex::scoped_lock lock(moved_nodes_mutex_);
  moved_nodes_.clear();
}

/*****************************************************************************/
void LoopClosureAssistant::addMovedNodes(const int& id, Eigen::Vector3d vec)
/*****************************************************************************/
{
  ROS_INFO("LoopClosureAssistant: Node %i new manual loop closure "
    "pose has been recorded.",id);
  boost::mutex::scoped_lock lock(moved_nodes_mutex_);
  moved_nodes_[id] = vec;
}

/*****************************************************************************/
bool LoopClosureAssistant::deleteEdge(const karto::Edge<karto::LocalizedRangeScan>* edge)
/*****************************************************************************/
{
  int edge_idx_on_source, edge_idx_on_target, edge_idx_in_graph;
  const kt_int32s source_id = edge->GetSource()->GetObject()->GetStateId();
  const kt_int32s target_id = edge->GetTarget()->GetObject()->GetStateId();

  {
    auto it = std::find(edge->GetSource()->GetEdges().begin(), edge->GetSource()->GetEdges().end(), edge);
    if (it != edge->GetSource()->GetEdges().end())
    {
      edge_idx_on_source = std::distance(edge->GetSource()->GetEdges().begin(), it);
    }
    else
    {
      ROS_ERROR("LoopClosureAssistant: Did not find edge on source "
        "node %i to delete.", source_id);
      return false;
    }
  }

  {
    auto it = std::find(edge->GetTarget()->GetEdges().begin(), edge->GetTarget()->GetEdges().end(), edge);
    if (it != edge->GetTarget()->GetEdges().end())
    {
      edge_idx_on_target = std::distance(edge->GetTarget()->GetEdges().begin(), it);
    }
    else
    {
      ROS_ERROR("LoopClosureAssistant: Did not find edge on target "
        "node %i to delete.", target_id);
      return false;
    }
  }

  {
    auto it = std::find(mapper_->GetGraph()->GetEdges().begin(), mapper_->GetGraph()->GetEdges().end(), edge);
    if (it != edge->GetTarget()->GetEdges().end())
    {
      edge_idx_in_graph = std::distance(mapper_->GetGraph()->GetEdges().begin(), it);
    }
    else
    {
      ROS_ERROR("LoopClosureAssistant: Did not find edge from %i to %i in graph"
        " to delete.", source_id, target_id);
      return false;
    }
  }

  edge->GetSource()->RemoveEdge(edge_idx_on_source);
  edge->GetTarget()->RemoveEdge(edge_idx_on_target);
  mapper_->GetGraph()->RemoveEdge(edge_idx_in_graph);
  mapper_->getScanSolver()->RemoveConstraint(source_id, target_id);

  return true;
}

} // end namespace