// This is an advanced implementation of the algorithm described in the
// following paper:
//   J. Zhang and S. Singh. LOAM: Lidar Odometry and Mapping in Real-time.
//     Robotics: Science and Systems Conference (RSS). Berkeley, CA, July 2014.

// Modifier: Livox               dev@livoxtech.com

// Copyright 2013, Ji Zhang, Carnegie Mellon University
// Further contributions copyright (c) 2016, Southwest Research Institute
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//#include "ros/package.h"
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from this
//    software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
#include <omp.h>
#include "IMU_Processing.hpp"
#include <ament_index_cpp/get_package_share_directory.hpp>
#include <unistd.h>
#include <Python.h>
#include <Eigen/Core>
#include <nav_msgs/msg/odometry.hpp>
#include <nav_msgs/msg/path.hpp>
#include <algorithm>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/io/pcd_io.h>
#include <tf2/LinearMath/Transform.h>
#include <tf2/LinearMath/Quaternion.h>
#include <tf2_ros/transform_broadcaster.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.hpp>
#include <geometry_msgs/msg/transform_stamped.hpp>
#include "preprocess.h"
#include <ikd-Tree/ikd_Tree.h>
#include <LI_init/LI_init.h>
#include <boost/filesystem.hpp>
#include <functional> // std::bind
#ifdef USE_LIVOX
#include <livox_ros_driver/CustomMsg.h>
#endif

#ifndef DEPLOY
#include "matplotlibcpp.h"
namespace plt = matplotlibcpp;
#endif

#define LASER_POINT_COV     (0.001)
#define MAXN                (720000)
#define PUBFRAME_PERIOD     (20)

float DET_RANGE = 300.0f;
const float MOV_THRESHOLD = 1.5f;

mutex mtx_buffer;
condition_variable sig_buffer;

string root_dir = ROOT_DIR;
string map_file_path, lid_topic, imu_topic;

int iterCount = 0, feats_down_size = 0, NUM_MAX_ITERATIONS = 0, laserCloudValidNum = 0, \
 effect_feat_num = 0, scan_count = 0, publish_count = 0;

double res_mean_last = 0.05;
double gyr_cov = 0.1, acc_cov = 0.1, grav_cov = 0.0001, b_gyr_cov = 0.0001, b_acc_cov = 0.0001;
double last_timestamp_lidar = 0, last_timestamp_imu = 0.0;
double filter_size_surf_min = 0, filter_size_map_min = 0;
double cube_len = 0, total_distance = 0, lidar_end_time = 0, first_lidar_time = 0.0;

// Time Log Variables
int kdtree_size_st = 0, kdtree_size_end = 0, add_point_size = 0;


int lidar_type, pcd_save_interval = -1, pcd_index = 0;
bool lidar_pushed, flg_reset, flg_exit = false, flg_EKF_inited = true;
bool imu_en = false;
bool scan_pub_en = false, dense_pub_en = false, scan_body_pub_en = false;
bool runtime_pos_log = false, pcd_save_en = false, extrinsic_est_en = true, path_en = true;

// LI-Init Parameters
bool cut_frame = true, data_accum_finished = false, data_accum_start = false, online_calib_finish = false, refine_print = false;
int cut_frame_num = 1, orig_odom_freq = 10, frame_num = 0;
double time_lag_IMU_wtr_lidar = 0.0, move_start_time = 0.0, online_calib_starts_time = 0.0, mean_acc_norm = 9.81;
double online_refine_time = 20.0; //unit: s
vector<double> Trans_LI_cov(3, 0.0005);
vector<double> Rot_LI_cov(3, 0.00005);
V3D mean_acc = Zero3d;
ofstream fout_result;


vector<BoxPointType> cub_needrm;
deque<PointCloudXYZI::Ptr> lidar_buffer;
deque<double> time_buffer;
deque<sensor_msgs::msg::Imu::SharedPtr> imu_buffer;
vector<vector<int>> pointSearchInd_surf;
vector<PointVector> Nearest_Points;
bool point_selected_surf[100000] = {0};
float res_last[100000] = {0.0};
double total_residual;

//surf feature in map
PointCloudXYZI::Ptr featsFromMap(new PointCloudXYZI());
PointCloudXYZI::Ptr feats_undistort(new PointCloudXYZI());
PointCloudXYZI::Ptr feats_down_body(new PointCloudXYZI());
PointCloudXYZI::Ptr feats_down_world(new PointCloudXYZI());
PointCloudXYZI::Ptr normvec(new PointCloudXYZI(100000, 1));
PointCloudXYZI::Ptr laserCloudOri(new PointCloudXYZI(100000, 1));
PointCloudXYZI::Ptr corr_normvect(new PointCloudXYZI(100000, 1));
PointCloudXYZI::Ptr _featsArray;

pcl::VoxelGrid<PointType> downSizeFilterSurf;
pcl::VoxelGrid<PointType> downSizeFilterMap;

KD_TREE ikdtree;

M3D last_rot(M3D::Zero());
V3F XAxisPoint_body(LIDAR_SP_LEN, 0.0, 0.0);
V3F XAxisPoint_world(LIDAR_SP_LEN, 0.0, 0.0);
V3D euler_cur;
V3D position_last(Zero3d);
V3D last_odom(Zero3d);


//estimator inputs and output;
MeasureGroup Measures;
StatesGroup state;

PointCloudXYZI::Ptr pcl_wait_save(new PointCloudXYZI());
pcl::PCDWriter pcd_writer;
string all_points_dir;

nav_msgs::msg::Path path;
nav_msgs::msg::Odometry odomAftMapped;
geometry_msgs::msg::Quaternion geoQuat;
geometry_msgs::msg::PoseStamped msg_body_pose;

shared_ptr<Preprocess> p_pre(new Preprocess());
shared_ptr<LI_Init> Init_LI(new LI_Init());

#ifdef USE_LIVOX
rclcpp::Subscription<livox_ros_driver2::msg::CustomMsg>::SharedPtr sub_pcl_livox_;
#else
rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr sub_pcl_pc_;
#endif
rclcpp::Subscription<sensor_msgs::msg::Imu>::SharedPtr sub_imu;
rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr pubLaserCloudFullRes;
rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr pubLaserCloudFullRes_body;
rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr pubLaserCloudEffect;
rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr pubLaserCloudMap;
rclcpp::Publisher<nav_msgs::msg::Odometry>::SharedPtr pubOdomAftMapped;
rclcpp::Publisher<nav_msgs::msg::Path>::SharedPtr pubPath;
std::unique_ptr<tf2_ros::TransformBroadcaster> tf_broadcaster;

float calc_dist(PointType p1, PointType p2) {
    float d = (p1.x - p2.x) * (p1.x - p2.x) + (p1.y - p2.y) * (p1.y - p2.y) + (p1.z - p2.z) * (p1.z - p2.z);
    return d;
}

void calcBodyVar(Eigen::Vector3d &pb, const float range_inc,
                 const float degree_inc, Eigen::Matrix3d &var) {
    float range = sqrt(pb[0] * pb[0] + pb[1] * pb[1] + pb[2] * pb[2]);
    float range_var = range_inc * range_inc;
    Eigen::Matrix2d direction_var;
    direction_var << pow(sin(DEG2RAD(degree_inc)), 2), 0, 0,
            pow(sin(DEG2RAD(degree_inc)), 2);
    Eigen::Vector3d direction(pb);
    direction.normalize();
    Eigen::Matrix3d direction_hat;
    direction_hat << 0, -direction(2), direction(1), direction(2), 0,
            -direction(0), -direction(1), direction(0), 0;
    Eigen::Vector3d base_vector1(1, 1,
                                 -(direction(0) + direction(1)) / direction(2));
    base_vector1.normalize();
    Eigen::Vector3d base_vector2 = base_vector1.cross(direction);
    base_vector2.normalize();
    Eigen::Matrix<double, 3, 2> N;
    N << base_vector1(0), base_vector2(0), base_vector1(1), base_vector2(1),
            base_vector1(2), base_vector2(2);
    Eigen::Matrix<double, 3, 2> A = range * direction_hat * N;
    var = direction * range_var * direction.transpose() +
          A * direction_var * A.transpose();
}

void SigHandle(int sig) {
    if (pcd_save_en && pcd_save_interval < 0){
        all_points_dir = string(root_dir + "/PCD/PCD_all" + string(".pcd"));
        pcd_writer.writeBinary(all_points_dir, *pcl_wait_save);
    }
    flg_exit = true;
    RCLCPP_WARN(rclcpp::get_logger("laserMapping"), "catch sig %d", sig);
    sig_buffer.notify_all();
}

inline void dump_lio_state_to_log(FILE *fp) {
    V3D rot_ang(Log(state.rot_end));
    fprintf(fp, "%lf ", Measures.lidar_beg_time - first_lidar_time);
    fprintf(fp, "%lf %lf %lf ", rot_ang(0), rot_ang(1), rot_ang(2));                   // Angle
    fprintf(fp, "%lf %lf %lf ", state.pos_end(0), state.pos_end(1), state.pos_end(2)); // Pos
    fprintf(fp, "%lf %lf %lf ", state.vel_end(0), state.vel_end(1), state.vel_end(2)); // Vel
    fprintf(fp, "%lf %lf %lf ", 0.0, 0.0, 0.0);                                        // omega
    fprintf(fp, "%lf %lf %lf ", 0.0, 0.0, 0.0);                                        // Acc
    fprintf(fp, "%lf %lf %lf ", state.bias_g(0), state.bias_g(1), state.bias_g(2));    // Bias_g
    fprintf(fp, "%lf %lf %lf ", state.bias_a(0), state.bias_a(1), state.bias_a(2));    // Bias_a
    fprintf(fp, "%lf %lf %lf ", state.gravity(0), state.gravity(1), state.gravity(2)); // Bias_a  
    fprintf(fp, "\r\n");
    fflush(fp);
}


void pointBodyToWorld(PointType const *const pi, PointType *const po) {
    V3D p_body(pi->x, pi->y, pi->z);
    V3D p_global(state.rot_end * (state.offset_R_L_I * p_body + state.offset_T_L_I) + state.pos_end);

    po->x = p_global(0);
    po->y = p_global(1);
    po->z = p_global(2);
    po->normal_x = pi->normal_x;
    po->normal_y = pi->normal_y;
    po->normal_z = pi->normal_z;
    po->intensity = pi->intensity;
}

template<typename T>
void pointBodyToWorld(const Matrix<T, 3, 1> &pi, Matrix<T, 3, 1> &po) {
    V3D p_body(pi[0], pi[1], pi[2]);
    V3D p_global(state.rot_end * (state.offset_R_L_I * p_body + state.offset_T_L_I) + state.pos_end);
    po[0] = p_global(0);
    po[1] = p_global(1);
    po[2] = p_global(2);
}

void RGBpointBodyToWorld(PointType const *const pi, PointTypeRGB *const po) {
    V3D p_body(pi->x, pi->y, pi->z);
    V3D p_global(state.rot_end * (state.offset_R_L_I * p_body + state.offset_T_L_I) + state.pos_end);
    po->x = p_global(0);
    po->y = p_global(1);
    po->z = p_global(2);
    po->r = pi->normal_x;
    po->g = pi->normal_y;
    po->b = pi->normal_z;

    float intensity = pi->intensity;
    intensity = intensity - floor(intensity);

    int reflection_map = intensity * 10000;
}

int points_cache_size = 0;

void points_cache_collect() {
    PointVector points_history;
    ikdtree.acquire_removed_points(points_history);
    points_cache_size = points_history.size();
    for (int i = 0; i < points_history.size(); i++) _featsArray->push_back(points_history[i]);
}


BoxPointType LocalMap_Points;
bool Localmap_Initialized = false;

void lasermap_fov_segment() {
    cub_needrm.clear();

    pointBodyToWorld(XAxisPoint_body, XAxisPoint_world);
    V3D pos_LiD = state.pos_end;

    if (!Localmap_Initialized) {
        for (int i = 0; i < 3; i++) {
            LocalMap_Points.vertex_min[i] = pos_LiD(i) - cube_len / 2.0;
            LocalMap_Points.vertex_max[i] = pos_LiD(i) + cube_len / 2.0;
        }
        Localmap_Initialized = true;
        return;
    }

    float dist_to_map_edge[3][2];
    bool need_move = false;
    for (int i = 0; i < 3; i++) {
        dist_to_map_edge[i][0] = fabs(pos_LiD(i) - LocalMap_Points.vertex_min[i]);
        dist_to_map_edge[i][1] = fabs(pos_LiD(i) - LocalMap_Points.vertex_max[i]);
        if (dist_to_map_edge[i][0] <= MOV_THRESHOLD * DET_RANGE ||
            dist_to_map_edge[i][1] <= MOV_THRESHOLD * DET_RANGE)
            need_move = true;
    }
    if (!need_move) return;
    BoxPointType New_LocalMap_Points, tmp_boxpoints;
    New_LocalMap_Points = LocalMap_Points;
    float mov_dist = max((cube_len - 2.0 * MOV_THRESHOLD * DET_RANGE) * 0.5 * 0.9,
                         double(DET_RANGE * (MOV_THRESHOLD - 1)));
    for (int i = 0; i < 3; i++) {
        tmp_boxpoints = LocalMap_Points;
        if (dist_to_map_edge[i][0] <= MOV_THRESHOLD * DET_RANGE) {
            New_LocalMap_Points.vertex_max[i] -= mov_dist;
            New_LocalMap_Points.vertex_min[i] -= mov_dist;
            tmp_boxpoints.vertex_min[i] = LocalMap_Points.vertex_max[i] - mov_dist;
            cub_needrm.push_back(tmp_boxpoints);
        } else if (dist_to_map_edge[i][1] <= MOV_THRESHOLD * DET_RANGE) {
            New_LocalMap_Points.vertex_max[i] += mov_dist;
            New_LocalMap_Points.vertex_min[i] += mov_dist;
            tmp_boxpoints.vertex_max[i] = LocalMap_Points.vertex_min[i] + mov_dist;
            cub_needrm.push_back(tmp_boxpoints);
        }
    }
    LocalMap_Points = New_LocalMap_Points;
    points_cache_collect();
}

double timediff_imu_wrt_lidar = 0.0;
bool timediff_set_flg = false;

void standard_pcl_cbk(const sensor_msgs::msg::PointCloud2::UniquePtr msg) {
    mtx_buffer.lock();
    scan_count++;
    if (get_time_sec(msg->header.stamp) < last_timestamp_lidar) {
        RCLCPP_ERROR(rclcpp::get_logger("laserMapping"),"lidar loop back, clear Lidar buffer.");
        lidar_buffer.clear();
        time_buffer.clear();
    }

    last_timestamp_lidar = get_time_sec(msg->header.stamp);
    if (abs(last_timestamp_imu - last_timestamp_lidar) > 1.0 && !timediff_set_flg && !imu_buffer.empty()) {
        timediff_set_flg = true;
        timediff_imu_wrt_lidar = last_timestamp_imu - last_timestamp_lidar;
        printf("Self sync IMU and LiDAR, HARD time lag is %.10lf \n \n", timediff_imu_wrt_lidar);
    }

    if ((lidar_type == VELO || lidar_type == OUSTER || lidar_type == PANDAR || lidar_type == ROBOSENSE) && cut_frame) {
        deque<PointCloudXYZI::Ptr> ptr;
        deque<double> timestamp_lidar;
        p_pre->process_cut_frame_pcl2(msg, ptr, timestamp_lidar, cut_frame_num, scan_count);
        while (!ptr.empty() && !timestamp_lidar.empty()) {
            lidar_buffer.push_back(ptr.front());
            ptr.pop_front();
            time_buffer.push_back(timestamp_lidar.front() / double(1000));//unit:s
            timestamp_lidar.pop_front();
        }
    } else {
        PointCloudXYZI::Ptr ptr(new PointCloudXYZI());
        p_pre->process(msg, ptr);
        lidar_buffer.push_back(ptr);
        time_buffer.push_back(get_time_sec(msg->header.stamp));
    }
    mtx_buffer.unlock();
    sig_buffer.notify_all();
}

#ifdef USE_LIVOX
void livox_pcl_cbk(const livox_ros_driver2::msg::CustomMsg::UniquePtr msg) 
{
    mtx_buffer.lock();
    double cur_time = get_time_sec(msg->header.stamp);
    double preprocess_start_time = omp_get_wtime();
    scan_count ++;
    if (!is_first_lidar && cur_time < last_timestamp_lidar)
    {
        std::cerr << "lidar loop back, clear buffer" << std::endl;
        lidar_buffer.clear();
    }
    if(is_first_lidar)
    {
        is_first_lidar = false;
    }
    last_timestamp_lidar = cur_time;
    
    if (!time_sync_en && abs(last_timestamp_imu - last_timestamp_lidar) > 10.0 && !imu_buffer.empty() && !lidar_buffer.empty() )
    {
        printf("IMU and LiDAR not Synced, IMU time: %lf, lidar header time: %lf \n",last_timestamp_imu, last_timestamp_lidar);
    }

    if (time_sync_en && !timediff_set_flg && abs(last_timestamp_lidar - last_timestamp_imu) > 1 && !imu_buffer.empty())
    {
        timediff_set_flg = true;
        timediff_lidar_wrt_imu = last_timestamp_lidar + 0.1 - last_timestamp_imu;
        printf("Self sync IMU and LiDAR, time diff is %.10lf \n", timediff_lidar_wrt_imu);
    }

    PointCloudXYZI::Ptr  ptr(new PointCloudXYZI());
    p_pre->process(msg, ptr);
    lidar_buffer.push_back(ptr);
    time_buffer.push_back(last_timestamp_lidar);
    
    s_plot11[scan_count] = omp_get_wtime() - preprocess_start_time;
    mtx_buffer.unlock();
    sig_buffer.notify_all();
}
#endif 

void imu_cbk(const sensor_msgs::msg::Imu::UniquePtr msg_in) {
    publish_count++;
    mtx_buffer.lock();


    static double IMU_period, time_msg_in, last_time_msg_in;
    static int imu_cnt = 0;
    time_msg_in = get_time_sec(msg_in->header.stamp);


    if (imu_cnt < 100) {
        imu_cnt++;
        mean_acc += (V3D(msg_in->linear_acceleration.x, msg_in->linear_acceleration.y, msg_in->linear_acceleration.z) -
                     mean_acc) / (imu_cnt);
        if (imu_cnt > 1) {
            IMU_period += (time_msg_in - last_time_msg_in - IMU_period) / (imu_cnt - 1);
        }
        if (imu_cnt == 99) {
            cout << endl << "Acceleration norm  : " << mean_acc.norm() << endl;
            if (IMU_period > 0.01) {
                cout << "IMU data frequency : " << 1 / IMU_period << " Hz" << endl;
                RCLCPP_WARN(rclcpp::get_logger("laserMapping"), "IMU data frequency too low. Higher than 150 Hz is recommended.");
            }
            cout << endl;
        }
    }
    last_time_msg_in = time_msg_in;


    sensor_msgs::msg::Imu::SharedPtr msg(new sensor_msgs::msg::Imu(*msg_in));

    //IMU Time Compensation
    msg->header.stamp = rclcpp::Time(msg->header.stamp) - rclcpp::Duration::from_seconds(timediff_imu_wrt_lidar - time_lag_IMU_wtr_lidar);
    double timestamp = get_time_sec(msg->header.stamp);

    if (timestamp < last_timestamp_imu) {
        RCLCPP_WARN(rclcpp::get_logger("laserMapping"), "IMU loop back, clear IMU buffer.");
        imu_buffer.clear();
        Init_LI->IMU_buffer_clear();
    }

    last_timestamp_imu = timestamp;
    imu_buffer.push_back(msg);

    // push all IMU meas into Init_LI
    if (!imu_en && !data_accum_finished)
        Init_LI->push_ALL_IMU_CalibState(msg, mean_acc_norm);

    mtx_buffer.unlock();
    sig_buffer.notify_all();
}

bool sync_packages(MeasureGroup &meas) {
    if (lidar_buffer.empty() || imu_buffer.empty()){
        return false;
    }


    /** push a lidar scan **/
    if (!lidar_pushed) {
        meas.lidar = lidar_buffer.front();

        if (meas.lidar->points.size() <= 1) {
            RCLCPP_WARN(rclcpp::get_logger("laserMapping"), "Too few input point cloud!\n");
            lidar_buffer.pop_front();
            time_buffer.pop_front();
            return false;
        }

        meas.lidar_beg_time = time_buffer.front(); //unit:s

        if (lidar_type == L515)
            lidar_end_time = meas.lidar_beg_time;
        else
            lidar_end_time = meas.lidar_beg_time + meas.lidar->points.back().curvature / double(1000); //unit:s

        lidar_pushed = true;
    }

    if (last_timestamp_imu < lidar_end_time)
        return false;


    /** push imu data, and pop from imu buffer **/
    double imu_time = get_time_sec(imu_buffer.front()->header.stamp);
    meas.imu.clear();
    while ((!imu_buffer.empty()) && (imu_time < lidar_end_time)) {
        imu_time = get_time_sec(imu_buffer.front()->header.stamp);
        if (imu_time > lidar_end_time) break;
        meas.imu.push_back(imu_buffer.front());
        imu_buffer.pop_front();
    }
    lidar_buffer.pop_front();
    time_buffer.pop_front();
    lidar_pushed = false;
    return true;
}

bool sync_packages_only_lidar(MeasureGroup &meas) {
    if (lidar_buffer.empty())
        return false;

    /** push a lidar scan **/
    if (!lidar_pushed) {
        meas.lidar = lidar_buffer.front();

        if (meas.lidar->points.size() <= 1) {
            RCLCPP_WARN(rclcpp::get_logger("laserMapping"), "Too few input point cloud!\n");
            lidar_buffer.pop_front();
            time_buffer.pop_front();
            return false;
        }

        meas.lidar_beg_time = time_buffer.front(); //unit:s

        if (lidar_type == L515)
            lidar_end_time = meas.lidar_beg_time;
        else
            lidar_end_time = meas.lidar_beg_time + meas.lidar->points.back().curvature / double(1000); //unit:s

        lidar_pushed = true;
    }

    lidar_buffer.pop_front();
    time_buffer.pop_front();
    lidar_pushed = false;
    return true;
}


int process_increments = 0;

void map_incremental() {
    PointVector PointToAdd;
    PointVector PointNoNeedDownsample;
    PointToAdd.reserve(feats_down_size);
    PointNoNeedDownsample.reserve(feats_down_size);
    for (int i = 0; i < feats_down_size; i++) {
        /* transform to world frame */
        pointBodyToWorld(&(feats_down_body->points[i]), &(feats_down_world->points[i]));
        /* decide if need add to map */
        if (!Nearest_Points[i].empty() && flg_EKF_inited) {
            const PointVector &points_near = Nearest_Points[i];
            bool need_add = true;
            BoxPointType Box_of_Point;
            PointType downsample_result, mid_point;
            mid_point.x = floor(feats_down_world->points[i].x / filter_size_map_min) * filter_size_map_min +
                          0.5 * filter_size_map_min;
            mid_point.y = floor(feats_down_world->points[i].y / filter_size_map_min) * filter_size_map_min +
                          0.5 * filter_size_map_min;
            mid_point.z = floor(feats_down_world->points[i].z / filter_size_map_min) * filter_size_map_min +
                          0.5 * filter_size_map_min;
            float dist = calc_dist(feats_down_world->points[i], mid_point);
            if (fabs(points_near[0].x - mid_point.x) > 0.5 * filter_size_map_min &&
                fabs(points_near[0].y - mid_point.y) > 0.5 * filter_size_map_min &&
                fabs(points_near[0].z - mid_point.z) > 0.5 * filter_size_map_min) {
                PointNoNeedDownsample.push_back(feats_down_world->points[i]);
                continue;
            }
            for (int readd_i = 0; readd_i < NUM_MATCH_POINTS; readd_i++) {
                if (points_near.size() < NUM_MATCH_POINTS) break;
                if (calc_dist(points_near[readd_i], mid_point) < dist) {
                    need_add = false;
                    break;
                }
            }
            if (need_add) PointToAdd.push_back(feats_down_world->points[i]);
        } else {
            PointToAdd.push_back(feats_down_world->points[i]);
        }
    }

    add_point_size = ikdtree.Add_Points(PointToAdd, true);
    ikdtree.Add_Points(PointNoNeedDownsample, false);
    add_point_size = PointToAdd.size() + PointNoNeedDownsample.size();
}

void publish_frame_world(const rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr &pubLaserCloudFullRes) {
    if (scan_pub_en) {
        PointCloudXYZI::Ptr laserCloudFullRes(dense_pub_en ? feats_undistort : feats_down_body);
        int size = laserCloudFullRes->points.size();

        PointCloudXYZRGB::Ptr laserCloudWorldRGB(new PointCloudXYZRGB(size, 1));
        PointCloudXYZI::Ptr laserCloudWorld(new PointCloudXYZI(size, 1));

        for (int i = 0; i < size; i++) {
            if (lidar_type == L515)
                RGBpointBodyToWorld(&laserCloudFullRes->points[i], \
                                &laserCloudWorldRGB->points[i]);
            else
                pointBodyToWorld(&laserCloudFullRes->points[i], \
                                &laserCloudWorld->points[i]);
        }

        sensor_msgs::msg::PointCloud2 laserCloudmsg;
        if (lidar_type == L515)
            pcl::toROSMsg(*laserCloudWorldRGB, laserCloudmsg);
        else
            pcl::toROSMsg(*laserCloudWorld, laserCloudmsg);

        laserCloudmsg.header.stamp = get_ros_time(lidar_end_time); // Convert seconds to nanoseconds
        laserCloudmsg.header.frame_id = "camera_init";
        pubLaserCloudFullRes->publish(laserCloudmsg);
        publish_count -= PUBFRAME_PERIOD;
    }


    /**************** save map ****************/
    /* 1. make sure you have enough memories
       2. noted that pcd save will influence the real-time performences **/
    if (pcd_save_en) {
        boost::filesystem::create_directories(root_dir + "/PCD");
        int size = feats_undistort->points.size();
        PointCloudXYZI::Ptr laserCloudWorld(new PointCloudXYZI(size, 1));
        for (int i = 0; i < size; i++) {
            pointBodyToWorld(&feats_undistort->points[i], &laserCloudWorld->points[i]);
        }

        *pcl_wait_save += *laserCloudWorld;
        static int scan_wait_num = 0;
        scan_wait_num++;
        if (pcl_wait_save->size() > 0 && pcd_save_interval > 0 && scan_wait_num >= pcd_save_interval) {
            pcd_index++;
            all_points_dir = string(root_dir + "/PCD/PCD") + to_string(pcd_index) + string(".pcd");
            cout << "current scan saved to " << all_points_dir << endl;
            pcd_writer.writeBinary(all_points_dir, *pcl_wait_save);
            pcl_wait_save->clear();
            scan_wait_num = 0;
        }
    }
}

void publish_frame_body(const rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr &pubLaserCloudFullRes_body) {
    PointCloudXYZI::Ptr laserCloudFullRes(feats_undistort);
    sensor_msgs::msg::PointCloud2 laserCloudmsg;
    pcl::toROSMsg(*feats_undistort, laserCloudmsg);
    laserCloudmsg.header.stamp = get_ros_time(lidar_end_time); // Convert seconds to nanoseconds
    laserCloudmsg.header.frame_id = "camera_init";
    pubLaserCloudFullRes_body->publish(laserCloudmsg);
}

void publish_effect_world(const rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr &pubLaserCloudEffect) {
    PointCloudXYZI::Ptr laserCloudWorld(new PointCloudXYZI(effect_feat_num, 1));
    for (int i = 0; i < effect_feat_num; i++) {
        pointBodyToWorld(&laserCloudOri->points[i], &laserCloudWorld->points[i]);
    }
    sensor_msgs::msg::PointCloud2 laserCloudFullRes3;
    pcl::toROSMsg(*laserCloudWorld, laserCloudFullRes3);
    laserCloudFullRes3.header.stamp = get_ros_time(lidar_end_time); // Convert seconds to nanoseconds
    laserCloudFullRes3.header.frame_id = "camera_init";
    pubLaserCloudEffect->publish(laserCloudFullRes3);
}

void publish_map(const rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr &pubLaserCloudMap) {
    sensor_msgs::msg::PointCloud2 laserCloudMap;
    pcl::toROSMsg(*featsFromMap, laserCloudMap);
    laserCloudMap.header.stamp = rclcpp::Time(lidar_end_time); // Convert seconds to nanoseconds
    laserCloudMap.header.frame_id = "camera_init";
    pubLaserCloudMap->publish(laserCloudMap);
}

template<typename T>
void set_posestamp(T &out) {
    if (!imu_en) {
        out.position.x = state.pos_end(0);
        out.position.y = state.pos_end(1);
        out.position.z = state.pos_end(2);
    } else {
        //Pubulish LiDAR's pose and position
        V3D pos_cur_lidar = state.rot_end * state.offset_T_L_I + state.pos_end;
        out.position.x = pos_cur_lidar(0);
        out.position.y = pos_cur_lidar(1);
        out.position.z = pos_cur_lidar(2);
    }
    out.orientation.x = geoQuat.x;
    out.orientation.y = geoQuat.y;
    out.orientation.z = geoQuat.z;
    out.orientation.w = geoQuat.w;
}

void publish_odometry(const rclcpp::Publisher<nav_msgs::msg::Odometry>::SharedPtr pubOdomAftMapped,
                      std::unique_ptr<tf2_ros::TransformBroadcaster> &br) {
    odomAftMapped.header.frame_id = "camera_init";
    odomAftMapped.child_frame_id = "aft_mapped";
    odomAftMapped.header.stamp = rclcpp::Time(lidar_end_time); 
    set_posestamp(odomAftMapped.pose.pose);

    pubOdomAftMapped->publish(odomAftMapped);

    geometry_msgs::msg::TransformStamped transformStamped;
    transformStamped.header.stamp = odomAftMapped.header.stamp;
    transformStamped.header.frame_id = "camera_init";
    transformStamped.child_frame_id = "aft_mapped";

    transformStamped.transform.translation.x = odomAftMapped.pose.pose.position.x;
    transformStamped.transform.translation.y = odomAftMapped.pose.pose.position.y;
    transformStamped.transform.translation.z = odomAftMapped.pose.pose.position.z;

    transformStamped.transform.rotation.w = odomAftMapped.pose.pose.orientation.w;
    transformStamped.transform.rotation.x = odomAftMapped.pose.pose.orientation.x;
    transformStamped.transform.rotation.y = odomAftMapped.pose.pose.orientation.y;
    transformStamped.transform.rotation.z = odomAftMapped.pose.pose.orientation.z;

    br->sendTransform(transformStamped);
}

// void publish_mavros(const rclcpp::Publisher<geometry_msgs::msg::PoseStamped>::SharedPtr &mavros_pose_publisher) {
//     msg_body_pose.header.stamp = get_ros_time(lidar_end_time); // Convert seconds to nanoseconds
//     msg_body_pose.header.frame_id = "camera_odom_frame";
//     set_posestamp(msg_body_pose.pose);
//     mavros_pose_publisher->publish(msg_body_pose);
// }

void publish_path(const rclcpp::Publisher<nav_msgs::msg::Path>::SharedPtr &pubPath) {
    set_posestamp(msg_body_pose.pose);
    msg_body_pose.header.stamp = get_ros_time(lidar_end_time); // Convert seconds to nanoseconds
    msg_body_pose.header.frame_id = "camera_init";
    static int jjj = 0;
    jjj++;
    if (jjj % 5 == 0) // if path is too large, the RVIZ will crash
    {
        path.poses.push_back(msg_body_pose);
        pubPath->publish(path);
    }
}

void fileout_calib_result() {
    fout_result.setf(ios::fixed);
    fout_result << setprecision(6)
                << "Rotation LiDAR to IMU (degree)     = " << RotMtoEuler(state.offset_R_L_I).transpose() * 57.3
                << endl;
    fout_result << "Translation LiDAR to IMU (meter)   = " << state.offset_T_L_I.transpose() << endl;
    fout_result << "Time Lag IMU to LiDAR (second)     = " << time_lag_IMU_wtr_lidar + timediff_imu_wrt_lidar << endl;
    fout_result << "Bias of Gyroscope  (rad/s)         = " << state.bias_g.transpose() << endl;
    fout_result << "Bias of Accelerometer (meters/s^2) = " << state.bias_a.transpose() << endl;
    fout_result << "Gravity in World Frame(meters/s^2) = " << state.gravity.transpose() << endl << endl;

    MD(4, 4) Transform;
    Transform.setIdentity();
    Transform.block<3, 3>(0, 0) = state.offset_R_L_I;
    Transform.block<3, 1>(0, 3) = state.offset_T_L_I;
    fout_result << "Homogeneous Transformation Matrix from LiDAR to IMU: " << endl;
    fout_result << Transform << endl << endl << endl;
}

void print_refine_result() {
    cout.setf(ios::fixed);
    cout << endl;
    printf(BOLDGREEN "[Final Result] " RESET);
    cout << setprecision(6)
         << "Rotation LiDAR to IMU    = " << RotMtoEuler(state.offset_R_L_I).transpose() * 57.3 << " deg" << endl;
    printf(BOLDGREEN "[Final Result] " RESET);
    cout << "Translation LiDAR to IMU = " << state.offset_T_L_I.transpose() << " m" << endl;
    printf(BOLDGREEN "[Final Result] " RESET);
    printf("Time Lag IMU to LiDAR    = %.8lf s \n", time_lag_IMU_wtr_lidar + timediff_imu_wrt_lidar);
    printf(BOLDGREEN "[Final Result] " RESET);
    cout << "Bias of Gyroscope        = " << state.bias_g.transpose() << " rad/s" << endl;
    printf(BOLDGREEN "[Final Result] " RESET);
    cout << "Bias of Accelerometer    = " << state.bias_a.transpose() << " m/s^2" << endl;
    printf(BOLDGREEN "[Final Result] " RESET);
    cout << "Gravity in World Frame   = " << state.gravity.transpose() << " m/s^2" << endl;
}

void printProgress(double percentage) {
    int val = (int) (percentage * 100);
    int lpad = (int) (percentage * PBWIDTH);
    int rpad = PBWIDTH - lpad;
    printf("\033[1A\r");
    printf(BOLDMAGENTA "[Refinement] ");
    if (percentage < 1) {
        printf(BOLDYELLOW "Online Refinement: ");
        printf(YELLOW "%3d%% [%.*s%*s]\n", val, lpad, PBSTR, rpad, "");
        cout << RESET;
    } else {
        printf(BOLDGREEN " Online Refinement ");
        printf(GREEN "%3d%% [%.*s%*s]\n", val, lpad, PBSTR, rpad, "");
        cout << RESET;
    }
    fflush(stdout);
}

void init_parameters(std::shared_ptr<rclcpp::Node> node)
{
    node->declare_parameter<int>("max_iteration", 4);
    node->declare_parameter<int>("point_filter_num", 2);
    node->declare_parameter<std::string>("map_file_path", "");
    node->declare_parameter<std::string>("common.lid_topic", "/livox/lidar");
    node->declare_parameter<std::string>("common.imu_topic", "/livox/imu");
    node->declare_parameter<double>("mapping.filter_size_surf", 0.5);
    node->declare_parameter<double>("mapping.filter_size_map", 0.5);
    node->declare_parameter<double>("cube_side_length", 200);
    node->declare_parameter<float>("mapping.det_range", 300.f);
    node->declare_parameter<double>("mapping.gyr_cov", 0.1);
    node->declare_parameter<double>("mapping.acc_cov", 0.1);
    node->declare_parameter<double>("mapping.grav_cov", 0.001);
    node->declare_parameter<double>("mapping.b_gyr_cov", 0.0001);
    node->declare_parameter<double>("mapping.b_acc_cov", 0.0001);
    node->declare_parameter<double>("preprocess.blind", 1.0);
    node->declare_parameter<int>("preprocess.lidar_type", AVIA);
    node->declare_parameter<int>("preprocess.scan_line", 16);
    node->declare_parameter<bool>("preprocess.feature_extract_en", false);
    node->declare_parameter<bool>("initialization.cut_frame", true);
    node->declare_parameter<int>("initialization.cut_frame_num", 1);
    node->declare_parameter<int>("initialization.orig_odom_freq", 10);
    node->declare_parameter<double>("initialization.online_refine_time", 20.0);
    node->declare_parameter<double>("initialization.mean_acc_norm", 9.81);
    node->declare_parameter<double>("initialization.data_accum_length", 300);
    node->declare_parameter<std::vector<double>>("initialization.Rot_LI_cov", std::vector<double>());
    node->declare_parameter<std::vector<double>>("initialization.Trans_LI_cov", std::vector<double>());
    node->declare_parameter<bool>("publish.path_en", true);
    node->declare_parameter<bool>("publish.scan_publish_en", true);
    node->declare_parameter<bool>("publish.dense_publish_en", true);
    node->declare_parameter<bool>("publish.scan_bodyframe_pub_en", true);
    node->declare_parameter<bool>("runtime_pos_log_enable", false);
    node->declare_parameter<bool>("pcd_save.pcd_save_en", false);
    node->declare_parameter<int>("pcd_save.interval", -1);

    node->get_parameter("max_iteration", NUM_MAX_ITERATIONS);
    node->get_parameter("point_filter_num", p_pre->point_filter_num);
    node->get_parameter("map_file_path", map_file_path);
    node->get_parameter("common.lid_topic", lid_topic);
    node->get_parameter("common.imu_topic", imu_topic);
    node->get_parameter("mapping.filter_size_surf", filter_size_surf_min);
    node->get_parameter("mapping.filter_size_map", filter_size_map_min);
    node->get_parameter("cube_side_length", cube_len);
    node->get_parameter("mapping.det_range", DET_RANGE);
    node->get_parameter("mapping.gyr_cov", gyr_cov);
    node->get_parameter("mapping.acc_cov", acc_cov);
    node->get_parameter("mapping.grav_cov", grav_cov);
    node->get_parameter("mapping.b_gyr_cov", b_gyr_cov);
    node->get_parameter("mapping.b_acc_cov", b_acc_cov);
    node->get_parameter("preprocess.blind", p_pre->blind);
    node->get_parameter("preprocess.lidar_type", lidar_type);
    node->get_parameter("preprocess.scan_line", p_pre->N_SCANS);
    node->get_parameter("preprocess.feature_extract_en", p_pre->feature_enabled);
    node->get_parameter("initialization.cut_frame", cut_frame);
    node->get_parameter("initialization.cut_frame_num", cut_frame_num);
    node->get_parameter("initialization.orig_odom_freq", orig_odom_freq);
    node->get_parameter("initialization.online_refine_time", online_refine_time);
    node->get_parameter("initialization.mean_acc_norm", mean_acc_norm);
    node->get_parameter("initialization.data_accum_length", Init_LI->data_accum_length);
    node->get_parameter("initialization.Rot_LI_cov", Rot_LI_cov);
    node->get_parameter("initialization.Trans_LI_cov", Trans_LI_cov);
    node->get_parameter("publish.path_en", path_en);
    node->get_parameter("publish.scan_publish_en", scan_pub_en);
    node->get_parameter("publish.dense_publish_en", dense_pub_en);
    node->get_parameter("publish.scan_bodyframe_pub_en", scan_body_pub_en);
    node->get_parameter("runtime_pos_log_enable", runtime_pos_log);
    node->get_parameter("pcd_save.pcd_save_en", pcd_save_en);
    node->get_parameter("pcd_save.interval", pcd_save_interval);
}

int main(int argc, char **argv) {
    rclcpp::init(argc, argv);
    std::shared_ptr<rclcpp::Node> node = std::make_shared<rclcpp::Node>("laserMapping");
    init_parameters(node);

    cout << "lidar_type: " << lidar_type << endl;
    cout << "LiDAR-only odometry starts." << endl;

    path.header.stamp = get_ros_time(lidar_end_time); // Convert seconds to nanoseconds
    path.header.frame_id = "camera_init";

    /*** variables definition ***/
    VD(DIM_STATE) solution;
    MD(DIM_STATE, DIM_STATE) G, H_T_H, I_STATE;
    V3D rot_add, T_add, vel_add, gyr_add;

    StatesGroup state_propagat;
    PointType pointOri, pointSel, coeff;

    double deltaT, deltaR;
    bool flg_EKF_converged, EKF_stop_flg = 0;

    _featsArray.reset(new PointCloudXYZI());


    memset(point_selected_surf, true, sizeof(point_selected_surf));
    memset(res_last, -1000.0f, sizeof(res_last));
    downSizeFilterSurf.setLeafSize(filter_size_surf_min, filter_size_surf_min, filter_size_surf_min);
    downSizeFilterMap.setLeafSize(filter_size_map_min, filter_size_map_min, filter_size_map_min);
    memset(point_selected_surf, true, sizeof(point_selected_surf));
    memset(res_last, -1000.0f, sizeof(res_last));

    shared_ptr<ImuProcess> p_imu(new ImuProcess());

    p_imu->lidar_type = p_pre->lidar_type = lidar_type;
    p_imu->imu_en = imu_en;
    p_imu->LI_init_done = false;
    p_imu->set_gyr_cov(V3D(gyr_cov, gyr_cov, gyr_cov));
    p_imu->set_acc_cov(V3D(acc_cov, acc_cov, acc_cov));
    p_imu->set_R_LI_cov(V3D(VEC_FROM_ARRAY(Rot_LI_cov)));
    p_imu->set_T_LI_cov(V3D(VEC_FROM_ARRAY(Trans_LI_cov)));
    p_imu->set_gyr_bias_cov(V3D(b_gyr_cov, b_gyr_cov, b_gyr_cov));
    p_imu->set_acc_bias_cov(V3D(b_acc_cov, b_acc_cov, b_acc_cov));


    G.setZero();
    H_T_H.setZero();
    I_STATE.setIdentity();


    // LI Init Related
    MatrixXd Jaco_rot(30000, 3);
    Jaco_rot.setZero();

    /*** debug record ***/
    boost::filesystem::create_directories(root_dir + "/Log");
    boost::filesystem::create_directories(root_dir + "/result");
    ofstream fout_out;
    fout_out.open(DEBUG_FILE_DIR("mat_out.txt"), ios::out);
    fout_result.open(RESULT_FILE_DIR("Initialization_result.txt"), ios::out);
    if (fout_out)
        cout << "~~~~" << ROOT_DIR << " file opened" << endl;
    else
        cout << "~~~~" << ROOT_DIR << " doesn't exist" << endl;


    /*** ROS subscribe initialization ***/
#ifdef USE_LIVOX
    if (p_pre->lidar_type == AVIA)
    {
        sub_pcl_livox_ = node->create_subscription<livox_ros_driver2::msg::CustomMsg>(lid_topic, rclcpp::SensorDataQoS(), livox_pcl_cbk);
    }
    else
#endif
    {
        std::cout << "\n lid_topic: " << lid_topic << std::endl;
        sub_pcl_pc_ = node->create_subscription<sensor_msgs::msg::PointCloud2>(lid_topic, rclcpp::SensorDataQoS(), standard_pcl_cbk);
    }

    sub_imu = node->create_subscription<sensor_msgs::msg::Imu>(imu_topic, 10, imu_cbk);
    pubLaserCloudFullRes = node->create_publisher<sensor_msgs::msg::PointCloud2>("/cloud_registered", 20);
    pubLaserCloudFullRes_body = node->create_publisher<sensor_msgs::msg::PointCloud2>("/cloud_registered_body", 20);
    pubLaserCloudEffect = node->create_publisher<sensor_msgs::msg::PointCloud2>("/cloud_effected", 20);
    pubLaserCloudMap = node->create_publisher<sensor_msgs::msg::PointCloud2>("/Laser_map", 20);
    pubOdomAftMapped = node->create_publisher<nav_msgs::msg::Odometry>("/aft_mapped_to_init", 20);
    pubPath = node->create_publisher<nav_msgs::msg::Path>("/path", 20);
    tf_broadcaster = std::make_unique<tf2_ros::TransformBroadcaster>(*node);


//------------------------------------------------------------------------------------------------------
    signal(SIGINT, SigHandle);
    rclcpp::Rate rate(5000);
    bool status = rclcpp::ok();

    while (status) {
        if (flg_exit) break;
        rclcpp::spin_some(node);
        if (sync_packages(Measures)) {
            if (flg_reset) {
                RCLCPP_WARN(rclcpp::get_logger("laserMapping"), "reset when rosbag play back.");
                p_imu->Reset();
                flg_reset = false;
                continue;
            }


            if (feats_undistort->empty() || (feats_undistort == NULL)) {
                first_lidar_time = Measures.lidar_beg_time;
                p_imu->first_lidar_time = first_lidar_time;
                RCLCPP_WARN(rclcpp::get_logger("laserMapping"), "LI-Init not ready, no points stored.");
            }

            p_imu->Process(Measures, state, feats_undistort);
            state_propagat = state;


            /*** Segment the map in lidar FOV ***/
            lasermap_fov_segment();

            /*** downsample the feature points in a scan ***/
            downSizeFilterSurf.setInputCloud(feats_undistort);
            downSizeFilterSurf.filter(*feats_down_body);
            feats_down_size = feats_down_body->points.size();
            /*** initialize the map kdtree ***/
            if (ikdtree.Root_Node == nullptr) {
                if (feats_down_size > 5) {
                    ikdtree.set_downsample_param(filter_size_map_min);
                    feats_down_world->resize(feats_down_size);
                    for (int i = 0; i < feats_down_size; i++) {
                        pointBodyToWorld(&(feats_down_body->points[i]), &(feats_down_world->points[i]));
                    }
                    ikdtree.Build(feats_down_world->points);
                }
                continue;
            }
            int featsFromMapNum = ikdtree.validnum();
            kdtree_size_st = ikdtree.size();


            /*** ICP and iterated Kalman filter update ***/
            normvec->resize(feats_down_size);
            feats_down_world->resize(feats_down_size);
            euler_cur = RotMtoEuler(state.rot_end);


            pointSearchInd_surf.resize(feats_down_size);
            Nearest_Points.resize(feats_down_size);
            int rematch_num = 0;
            bool nearest_search_en = true;


            /*** iterated state estimation ***/
            std::vector<M3D> body_var;
            std::vector<M3D> crossmat_list;
            body_var.reserve(feats_down_size);
            crossmat_list.reserve(feats_down_size);




            for (iterCount = 0; iterCount < NUM_MAX_ITERATIONS; iterCount++) {

                laserCloudOri->clear();
                corr_normvect->clear();
                total_residual = 0.0;

                /** closest surface search and residual computation **/
                #ifdef MP_EN
                    omp_set_num_threads(MP_PROC_NUM);
                    #pragma omp parallel for
                #endif
                for (int i = 0; i < feats_down_size; i++) {
                    PointType &point_body = feats_down_body->points[i];
                    PointType &point_world = feats_down_world->points[i];
                    V3D p_body(point_body.x, point_body.y, point_body.z);
                    /// transform to world frame
                    pointBodyToWorld(&point_body, &point_world);
                    vector<float> pointSearchSqDis(NUM_MATCH_POINTS);
                    auto &points_near = Nearest_Points[i];
                    uint8_t search_flag = 0;

                    if (nearest_search_en) {
                        /** Find the closest surfaces in the map **/
                        ikdtree.Nearest_Search(point_world, NUM_MATCH_POINTS, points_near, pointSearchSqDis, 5);
                        if (points_near.size() < NUM_MATCH_POINTS)
                            point_selected_surf[i] = false;
                        else
                            point_selected_surf[i] = !(pointSearchSqDis[NUM_MATCH_POINTS - 1] > 5);
                    }

                    res_last[i] = -1000.0f;

                    if (!point_selected_surf[i] || points_near.size() < NUM_MATCH_POINTS) {
                        point_selected_surf[i] = false;
                        continue;
                    }

                    point_selected_surf[i] = false;
                    VD(4) pabcd;
                    pabcd.setZero();
                    if (esti_plane(pabcd, points_near, 0.1)) //(planeValid)
                    {
                        float pd2 = pabcd(0) * point_world.x + pabcd(1) * point_world.y + pabcd(2) * point_world.z +
                                    pabcd(3);
                        float s = 1 - 0.9 * fabs(pd2) / sqrt(p_body.norm());

                        if (s > 0.9) {
                            point_selected_surf[i] = true;
                            normvec->points[i].x = pabcd(0);
                            normvec->points[i].y = pabcd(1);
                            normvec->points[i].z = pabcd(2);
                            normvec->points[i].intensity = pd2;
                            res_last[i] = abs(pd2);
                        }
                    }
                }
                effect_feat_num = 0;
                for (int i = 0; i < feats_down_size; i++) {
                    if (point_selected_surf[i]) {
                        laserCloudOri->points[effect_feat_num] = feats_down_body->points[i];
                        corr_normvect->points[effect_feat_num] = normvec->points[i];
                        effect_feat_num++;
                    }
                }

                res_mean_last = total_residual / effect_feat_num;

                /*** Computation of Measurement Jacobian matrix H and measurents vector ***/

                MatrixXd Hsub(effect_feat_num, 12);
                MatrixXd Hsub_T_R_inv(12, effect_feat_num);
                VectorXd R_inv(effect_feat_num);
                VectorXd meas_vec(effect_feat_num);

                Hsub.setZero();
                Hsub_T_R_inv.setZero();
                meas_vec.setZero();

                for (int i = 0; i < effect_feat_num; i++) {
                    const PointType &laser_p = laserCloudOri->points[i];
                    V3D point_this_L(laser_p.x, laser_p.y, laser_p.z);

                    V3D point_this = state.offset_R_L_I * point_this_L + state.offset_T_L_I;
                    M3D var;
                    calcBodyVar(point_this, 0.02, 0.05, var);
                    var = state.rot_end * var * state.rot_end.transpose();
                    M3D point_crossmat;
                    point_crossmat << SKEW_SYM_MATRX(point_this);

                    /*** get the normal vector of closest surface/corner ***/
                    const PointType &norm_p = corr_normvect->points[i];
                    V3D norm_vec(norm_p.x, norm_p.y, norm_p.z);

                    R_inv(i) = 1000;
                    laserCloudOri->points[i].intensity = sqrt(R_inv(i));

                    /*** calculate the Measurement Jacobian matrix H ***/
                    if (imu_en) {
                        M3D point_this_L_cross;
                        point_this_L_cross << SKEW_SYM_MATRX(point_this_L);
                        V3D H_R_LI = point_this_L_cross * state.offset_R_L_I.transpose() * state.rot_end.transpose() *
                                     norm_vec;
                        V3D H_T_LI = state.rot_end.transpose() * norm_vec;
                        V3D A(point_crossmat * state.rot_end.transpose() * norm_vec);
                        Hsub.row(i) << VEC_FROM_ARRAY(A), norm_p.x, norm_p.y, norm_p.z, VEC_FROM_ARRAY(
                                H_R_LI), VEC_FROM_ARRAY(H_T_LI);
                    } else {
                        V3D A(point_crossmat * state.rot_end.transpose() * norm_vec);
                        Hsub.row(i) << VEC_FROM_ARRAY(A), norm_p.x, norm_p.y, norm_p.z, 0, 0, 0, 0, 0, 0;
                    }

                    Hsub_T_R_inv.col(i) = Hsub.row(i).transpose() * 1000;
                    /*** Measurement: distance to the closest surface/corner ***/
                    meas_vec(i) = -norm_p.intensity;
                }

                MatrixXd K(DIM_STATE, effect_feat_num);

                EKF_stop_flg = false;
                flg_EKF_converged = false;

                /*** Iterative Kalman Filter Update ***/

                H_T_H.block<12, 12>(0, 0) = Hsub_T_R_inv * Hsub;
                MD(DIM_STATE, DIM_STATE) &&K_1 = (H_T_H + state.cov.inverse()).inverse();
                K = K_1.block<DIM_STATE, 12>(0, 0) * Hsub_T_R_inv;
                auto vec = state_propagat - state;
                solution = K * meas_vec + vec - K * Hsub * vec.block<12, 1>(0, 0);

                //state update
                state += solution;

                rot_add = solution.block<3, 1>(0, 0);
                T_add = solution.block<3, 1>(3, 0);


                if ((rot_add.norm() * 57.3 < 0.01) && (T_add.norm() * 100 < 0.015))
                    flg_EKF_converged = true;

                deltaR = rot_add.norm() * 57.3;
                deltaT = T_add.norm() * 100;

                euler_cur = RotMtoEuler(state.rot_end);

                /*** Rematch Judgement ***/
                nearest_search_en = false;
                if (flg_EKF_converged || ((rematch_num == 0) && (iterCount == (NUM_MAX_ITERATIONS - 2)))) {
                    nearest_search_en = true;
                    rematch_num++;
                }

                /*** Convergence Judgements and Covariance Update ***/
                if (!EKF_stop_flg && (rematch_num >= 2 || (iterCount == NUM_MAX_ITERATIONS - 1))) {
                    if (flg_EKF_inited) {
                        /*** Covariance Update ***/
                        G.setZero();
                        G.block<DIM_STATE, 12>(0, 0) = K * Hsub;
                        state.cov = (I_STATE - G) * state.cov;
                        total_distance += (state.pos_end - position_last).norm();
                        position_last = state.pos_end;

                        tf2::Quaternion quat;
                        if (!imu_en) {
                            // Generate quaternion from roll, pitch, yaw
                            quat.setRPY(euler_cur(0), euler_cur(1), euler_cur(2));
                        } else {
                            //Publish LiDAR's pose, instead of IMU's pose
                            M3D rot_cur_lidar = state.rot_end * state.offset_R_L_I;
                            V3D euler_cur_lidar = RotMtoEuler(rot_cur_lidar);
                            quat.setRPY(euler_cur_lidar(0), euler_cur_lidar(1), euler_cur_lidar(2));
                        }

                        // Convert tf2::Quaternion to geometry_msgs::msg::Quaternion
                        geoQuat = tf2::toMsg(quat);

                        VD(DIM_STATE) K_sum = K.rowwise().sum();
                        VD(DIM_STATE) P_diag = state.cov.diagonal();
                    }
                    EKF_stop_flg = true;
                }

                if (EKF_stop_flg) break;
            }

            /******* Publish odometry *******/
            publish_odometry(pubOdomAftMapped, tf_broadcaster);

            /*** add the feature points to map kdtree ***/
            map_incremental();

            kdtree_size_end = ikdtree.size();

            /***** Device starts to move, data accmulation begins. ****/
            if (!imu_en && !data_accum_start && state.pos_end.norm() > 0.05) {
                printf(BOLDCYAN "[Initialization] Movement detected, data accumulation starts.\n\n\n\n\n" RESET);
                data_accum_start = true;
                move_start_time = lidar_end_time;
            }

            /******* Publish points *******/
            if (scan_pub_en || pcd_save_en) publish_frame_world(pubLaserCloudFullRes);
            if (scan_pub_en && scan_body_pub_en) publish_frame_body(pubLaserCloudFullRes_body);
            last_odom = state.pos_end;
            last_rot = state.rot_end;
            publish_effect_world(pubLaserCloudEffect);
            if (path_en) publish_path(pubPath);
            //publish_mavros(mavros_pose_publisher);

            frame_num++;
            V3D ext_euler = RotMtoEuler(state.offset_R_L_I);
            fout_out << euler_cur.transpose() * 57.3 << " " << state.pos_end.transpose() << " "
                     << ext_euler.transpose() * 57.3 << " " \
                     << state.offset_T_L_I.transpose() << " " << state.vel_end.transpose() << " "  \
                     << " " << state.bias_g.transpose() << " " << state.bias_a.transpose() * 0.9822 / 9.81 << " "
                     << state.gravity.transpose() << " " << total_distance << endl;

            //Broadcast every second
            if (imu_en && frame_num % orig_odom_freq * cut_frame_num == 0 && !online_calib_finish) {
                double online_calib_completeness = lidar_end_time - online_calib_starts_time;
                online_calib_completeness =
                        online_calib_completeness < online_refine_time ? online_calib_completeness : online_refine_time;
                cout << "\x1B[2J\x1B[H"; //clear the screen
                if(online_refine_time > 0.1)
                    printProgress(online_calib_completeness / online_refine_time);
                if (!refine_print && online_calib_completeness > (online_refine_time - 1e-6)) {
                    refine_print = true;
                    online_calib_finish = true;
                    cout << endl;
                    print_refine_result();
                    fout_result << "Refinement result:" << endl;
                    fileout_calib_result();
                    std::string path = ament_index_cpp::get_package_share_directory("lidar_imu_init");
                    path += "/result/Initialization_result.txt";
                    cout << endl  << "Initialization and refinement result is written to " << endl << BOLDGREEN << path << RESET <<endl;
                }
            }


            if (!imu_en && !data_accum_finished && data_accum_start) {
                //Push Lidar's Angular velocity and linear velocity
                Init_LI->push_Lidar_CalibState(state.rot_end, state.bias_g, state.vel_end, lidar_end_time);
                //Data Accumulation Sufficience Appraisal
                data_accum_finished = Init_LI->data_sufficiency_assess(Jaco_rot, frame_num, state.bias_g,
                                                                       orig_odom_freq, cut_frame_num);

                if (data_accum_finished) {
                    Init_LI->LI_Initialization(orig_odom_freq, cut_frame_num, timediff_imu_wrt_lidar, move_start_time);

                    online_calib_starts_time = lidar_end_time;

                    //Transfer to FAST-LIO2
                    imu_en = true;
                    state.offset_R_L_I = Init_LI->get_R_LI();
                    state.offset_T_L_I = Init_LI->get_T_LI();
                    state.pos_end = -state.rot_end * state.offset_R_L_I.transpose() * state.offset_T_L_I +
                                    state.pos_end; //Body frame is IMU frame in FAST-LIO mode
                    state.rot_end = state.rot_end * state.offset_R_L_I.transpose();
                    state.gravity = Init_LI->get_Grav_L0();
                    state.bias_g = Init_LI->get_gyro_bias();
                    state.bias_a = Init_LI->get_acc_bias();


                    if (lidar_type != AVIA)
                        cut_frame_num = 2;

                    time_lag_IMU_wtr_lidar = Init_LI->get_total_time_lag(); //Compensate IMU's time in the buffer
                    for (int i = 0; i < imu_buffer.size(); i++) {
                        imu_buffer[i]->header.stamp = rclcpp::Time(imu_buffer[i]->header.stamp)- rclcpp::Duration::from_seconds(time_lag_IMU_wtr_lidar);
                    }

                    p_imu->imu_en = imu_en;
                    p_imu->LI_init_done = true;
                    p_imu->set_mean_acc_norm(mean_acc_norm);
                    p_imu->set_gyr_cov(V3D(0.1, 0.1, 0.1));
                    p_imu->set_acc_cov(V3D(0.1, 0.1, 0.1));
                    p_imu->set_gyr_bias_cov(V3D(0.0001, 0.0001, 0.0001));
                    p_imu->set_acc_bias_cov(V3D(0.0001, 0.0001, 0.0001));

                    //Output Initialization result
                    fout_result << "Initialization result:" << endl;
                    fileout_calib_result();
                }
            }
        }
        status = rclcpp::ok();
        rate.sleep();
    }

    cout << endl << REDPURPLE << "[Exit]: Exit the process." <<RESET <<endl;
    if (!online_calib_finish) {
        cout << YELLOW << "[WARN]: Online refinement not finished yet." << RESET;
        print_refine_result();
    }
    return 0;
}
