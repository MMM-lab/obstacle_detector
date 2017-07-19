/*
 * Software License Agreement (BSD License)
 *
 * Copyright (c) 2017, Poznan University of Technology
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Poznan University of Technology nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Author: Mateusz Przybyla
 */

#include "obstacle_detector/scans_merger.h"

using namespace obstacle_detector;
using namespace std;

ScansMerger::ScansMerger(ros::NodeHandle& nh, ros::NodeHandle& nh_local) : nh_(nh), nh_local_(nh_local) {
  p_active_ = false;

  front_scan_received_ = false;
  rear_scan_received_ = false;

  front_scan_error_ = false;
  rear_scan_error_ = false;

  params_srv_ = nh_local_.advertiseService("params", &ScansMerger::updateParams, this);

  initialize();
}

bool ScansMerger::updateParams(std_srvs::Empty::Request &req, std_srvs::Empty::Response &res) {
  bool prev_active = p_active_;

  nh_local_.param<bool>("active", p_active_, true);
  nh_local_.param<bool>("publish_scan", p_publish_scan_, true);
  nh_local_.param<bool>("publish_pcl", p_publish_pcl_, false);

  nh_local_.param<int>("ranges_num", p_ranges_num_, 1000);

  nh_local_.param<double>("min_scanner_range", p_min_scanner_range_, 0.05);
  nh_local_.param<double>("max_scanner_range", p_max_scanner_range_, 10.0);
  nh_local_.param<double>("max_x_range", p_max_x_range_,  10.0);
  nh_local_.param<double>("min_x_range", p_min_x_range_, -10.0);
  nh_local_.param<double>("max_y_range", p_max_y_range_,  10.0);
  nh_local_.param<double>("min_y_range", p_min_y_range_, -10.0);

  nh_local_.param<string>("fixed_frame_id", p_fixed_frame_id_, "map");
  nh_local_.param<string>("target_frame_id", p_target_frame_id_, "scanner_base");
  nh_local_.param<string>("front_scan_frame_id", p_front_scan_frame_id_, "front_scanner");
  nh_local_.param<string>("rear_scan_frame_id", p_rear_scan_frame_id_, "rear_scanner");

  if (p_active_ != prev_active) {
    if (p_active_) {
      front_scan_sub_ = nh_.subscribe("front_scan", 10, &ScansMerger::frontScanCallback, this);
      rear_scan_sub_ = nh_.subscribe("rear_scan", 10, &ScansMerger::rearScanCallback, this);
      scan_pub_ = nh_.advertise<sensor_msgs::LaserScan>("scan", 10);
      pcl_pub_ = nh_.advertise<sensor_msgs::PointCloud>("pcl", 10);
    }
    else {
      front_scan_sub_.shutdown();
      rear_scan_sub_.shutdown();
      scan_pub_.shutdown();
      pcl_pub_.shutdown();
    }
  }

  return true;
}

void ScansMerger::frontScanCallback(const sensor_msgs::LaserScan::ConstPtr front_scan) {
  if (!tf_ls_.waitForTransform(front_scan->header.frame_id, p_fixed_frame_id_,
                               front_scan->header.stamp + ros::Duration().fromSec(front_scan->ranges.size() * front_scan->time_increment), ros::Duration(1.0)))
    return;

  projector_.transformLaserScanToPointCloud(p_fixed_frame_id_, *front_scan, front_pcl_, tf_ls_);

  front_scan_received_ = true;
  front_scan_error_ = false;

  if (rear_scan_received_ || rear_scan_error_)
    publishMessages();
  else
    rear_scan_error_ = true;
}

void ScansMerger::rearScanCallback(const sensor_msgs::LaserScan::ConstPtr rear_scan) {
  if (!tf_ls_.waitForTransform(rear_scan->header.frame_id, p_fixed_frame_id_,
                               rear_scan->header.stamp + ros::Duration().fromSec(rear_scan->ranges.size() * rear_scan->time_increment), ros::Duration(1.0)))
    return;

  projector_.transformLaserScanToPointCloud(p_fixed_frame_id_, *rear_scan, rear_pcl_, tf_ls_);

  rear_scan_received_ = true;
  rear_scan_error_ = false;

  if (front_scan_received_ || front_scan_error_)
    publishMessages();
  else
    front_scan_error_ = true;
}

void ScansMerger::publishMessages() {
  ros::Time now = ros::Time::now();

  if (!tf_ls_.waitForTransform(p_target_frame_id_, now, front_pcl_.header.frame_id, front_pcl_.header.stamp, p_fixed_frame_id_, ros::Duration(1.0)))
    return;

  if (!tf_ls_.waitForTransform(p_target_frame_id_, now, rear_pcl_.header.frame_id, rear_pcl_.header.stamp, p_fixed_frame_id_, ros::Duration(1.0)))
    return;

  sensor_msgs::PointCloud new_front_pcl, new_rear_pcl;
  tf_ls_.transformPointCloud(p_target_frame_id_, now, front_pcl_, p_fixed_frame_id_, new_front_pcl);
  tf_ls_.transformPointCloud(p_target_frame_id_, now, rear_pcl_,  p_fixed_frame_id_, new_rear_pcl);

  if (p_publish_scan_) {
    sensor_msgs::LaserScanPtr scan_msg(new sensor_msgs::LaserScan);

    scan_msg->header.frame_id = p_target_frame_id_;
    scan_msg->header.stamp = now;
    scan_msg->angle_min = -M_PI;
    scan_msg->angle_max = M_PI;
    scan_msg->angle_increment = 2.0 * M_PI / (p_ranges_num_ - 1);
    scan_msg->time_increment = 0.0;
    scan_msg->scan_time = 0.1;
    scan_msg->range_min = p_min_scanner_range_;
    scan_msg->range_max = p_max_scanner_range_;

    vector<float> ranges;
    ranges.assign(p_ranges_num_, 2.0 * p_max_scanner_range_);

    for (auto& point : new_front_pcl.points) {
      float angle = atan2(point.y, point.x);
      float range = sqrt(pow(point.x, 2.0) + pow(point.y, 2.0));

      int idx = static_cast<int>(p_ranges_num_ * (angle + M_PI) / (2.0 * M_PI));

      if (ranges[idx] > range)
        ranges[idx] = range;
    }

    for (auto& point : new_rear_pcl.points) {
      float angle = atan2(point.y, point.x);
      float range = sqrt(pow(point.x, 2.0) + pow(point.y, 2.0));

      int idx = static_cast<int>(p_ranges_num_ * (angle + M_PI) / (2.0 * M_PI));

      if (ranges[idx] > range)
        ranges[idx] = range;
    }

    for (int jdx = 0; jdx < ranges.size(); ++jdx)
      if (ranges[jdx] < p_min_scanner_range_ || ranges[jdx] > p_max_scanner_range_)
        ranges[jdx] = nan("");

    scan_msg->ranges = ranges;

    scan_pub_.publish(scan_msg);
  }

  if (p_publish_pcl_) {
    sensor_msgs::PointCloudPtr pcl_msg(new sensor_msgs::PointCloud);

    pcl_msg->header.frame_id = p_target_frame_id_;
    pcl_msg->header.stamp = now;

    pcl_msg->points.insert(pcl_msg->points.end(), new_front_pcl.points.begin(), new_front_pcl.points.end());
    pcl_msg->points.insert(pcl_msg->points.end(), new_rear_pcl.points.begin(), new_rear_pcl.points.end());

    pcl_pub_.publish(pcl_msg);
  }

  front_scan_received_ = false;
  rear_scan_received_ = false;
}
